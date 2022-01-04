/*
 * Copyright 2011-2018 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "session/denoising.h"

#include "util/map.h"
#include "util/system.h"
#include "util/task.h"
#include "util/time.h"

#include <OpenImageIO/filesystem.h>

CCL_NAMESPACE_BEGIN

/* Utility Functions */

static void print_progress(int num, int total, int frame, int num_frames)
{
  const char *label = "Denoise Frame ";
  int cols = system_console_width();

  cols -= strlen(label);

  int len = 1;
  for (int x = total; x > 9; x /= 10) {
    len++;
  }

  int bars = cols - 2 * len - 6;

  printf("\r%s", label);

  if (num_frames > 1) {
    int frame_len = 1;
    for (int x = num_frames - 1; x > 9; x /= 10) {
      frame_len++;
    }
    bars -= frame_len + 2;
    printf("%*d ", frame_len, frame);
  }

  int v = int(float(num) * bars / total);
  printf("[");
  for (int i = 0; i < v; i++) {
    printf("=");
  }
  if (v < bars) {
    printf(">");
  }
  for (int i = v + 1; i < bars; i++) {
    printf(" ");
  }
  printf(string_printf("] %%%dd / %d", len, total).c_str(), num);
  fflush(stdout);
}

/* Splits in at its last dot, setting suffix to the part after the dot and in to the part before
 * it. Returns whether a dot was found. */
static bool split_last_dot(string &in, string &suffix)
{
  size_t pos = in.rfind(".");
  if (pos == string::npos) {
    return false;
  }
  suffix = in.substr(pos + 1);
  in = in.substr(0, pos);
  return true;
}

/* Separate channel names as generated by Blender.
 * If views is true:
 *   Inputs are expected in the form RenderLayer.Pass.View.Channel, sets renderlayer to
 *   "RenderLayer.View" Otherwise: Inputs are expected in the form RenderLayer.Pass.Channel */
static bool parse_channel_name(
    string name, string &renderlayer, string &pass, string &channel, bool multiview_channels)
{
  if (!split_last_dot(name, channel)) {
    return false;
  }
  string view;
  if (multiview_channels && !split_last_dot(name, view)) {
    return false;
  }
  if (!split_last_dot(name, pass)) {
    return false;
  }
  renderlayer = name;

  if (multiview_channels) {
    renderlayer += "." + view;
  }

  return true;
}

/* Channel Mapping */

struct ChannelMapping {
  int channel;
  string name;
};

static void fill_mapping(vector<ChannelMapping> &map, int pos, string name, string channels)
{
  for (const char *chan = channels.c_str(); *chan; chan++) {
    map.push_back({pos++, name + "." + *chan});
  }
}

static const int INPUT_NUM_CHANNELS = 13;
static const int INPUT_NOISY_IMAGE = 0;
static const int INPUT_DENOISING_NORMAL = 3;
static const int INPUT_DENOISING_ALBEDO = 6;
static const int INPUT_MOTION = 9;
static vector<ChannelMapping> input_channels()
{
  vector<ChannelMapping> map;
  fill_mapping(map, INPUT_NOISY_IMAGE, "Combined", "RGB");
  fill_mapping(map, INPUT_DENOISING_NORMAL, "Denoising Normal", "XYZ");
  fill_mapping(map, INPUT_DENOISING_ALBEDO, "Denoising Albedo", "RGB");
  fill_mapping(map, INPUT_MOTION, "Vector", "XYZW");
  return map;
}

static const int OUTPUT_NUM_CHANNELS = 3;
static vector<ChannelMapping> output_channels()
{
  vector<ChannelMapping> map;
  fill_mapping(map, 0, "Combined", "RGB");
  return map;
}

/* Renderlayer Handling */

bool DenoiseImageLayer::detect_denoising_channels()
{
  /* Map device input to image channels. */
  input_to_image_channel.clear();
  input_to_image_channel.resize(INPUT_NUM_CHANNELS, -1);

  for (const ChannelMapping &mapping : input_channels()) {
    vector<string>::iterator i = find(channels.begin(), channels.end(), mapping.name);
    if (i == channels.end()) {
      return false;
    }

    size_t input_channel = mapping.channel;
    size_t layer_channel = i - channels.begin();
    input_to_image_channel[input_channel] = layer_to_image_channel[layer_channel];
  }

  /* Map device output to image channels. */
  output_to_image_channel.clear();
  output_to_image_channel.resize(OUTPUT_NUM_CHANNELS, -1);

  for (const ChannelMapping &mapping : output_channels()) {
    vector<string>::iterator i = find(channels.begin(), channels.end(), mapping.name);
    if (i == channels.end()) {
      return false;
    }

    size_t output_channel = mapping.channel;
    size_t layer_channel = i - channels.begin();
    output_to_image_channel[output_channel] = layer_to_image_channel[layer_channel];
  }

  /* Check that all buffer channels are correctly set. */
  for (int i = 0; i < INPUT_NUM_CHANNELS; i++) {
    assert(input_to_image_channel[i] >= 0);
  }
  for (int i = 0; i < OUTPUT_NUM_CHANNELS; i++) {
    assert(output_to_image_channel[i] >= 0);
  }

  return true;
}

bool DenoiseImageLayer::match_channels(const std::vector<string> &channelnames,
                                       const std::vector<string> &neighbor_channelnames)
{
  vector<int> &mapping = previous_output_to_image_channel;

  assert(mapping.size() == 0);
  mapping.resize(output_to_image_channel.size(), -1);

  for (int i = 0; i < output_to_image_channel.size(); i++) {
    const string &channel = channelnames[output_to_image_channel[i]];
    std::vector<string>::const_iterator frame_channel = find(
        neighbor_channelnames.begin(), neighbor_channelnames.end(), channel);

    if (frame_channel == neighbor_channelnames.end()) {
      return false;
    }

    mapping[i] = frame_channel - neighbor_channelnames.begin();
  }

  return true;
}

/* Denoise Task */

DenoiseTask::DenoiseTask(Device *device, DenoiserPipeline *denoiser, int frame)
    : denoiser(denoiser), device(device), frame(frame), current_layer(0), buffers(device)
{
}

DenoiseTask::~DenoiseTask()
{
  free();
}

/* Denoiser Operations */

bool DenoiseTask::load_input_pixels(int layer)
{
  /* Load center image */
  DenoiseImageLayer &image_layer = image.layers[layer];

  float *buffer_data = buffers.buffer.data();
  image.read_pixels(image_layer, buffers.params, buffer_data);

  /* Load previous image */
  if (frame > 0 && !image.read_previous_pixels(image_layer, buffers.params, buffer_data)) {
    error = "Failed to read neighbor frame pixels";
    return false;
  }

  /* Copy to device */
  buffers.buffer.copy_to_device();

  return true;
}

/* Task stages */

static void add_pass(vector<Pass *> &passes, PassType type, PassMode mode = PassMode::NOISY)
{
  Pass *pass = new Pass();
  pass->set_type(type);
  pass->set_mode(mode);

  passes.push_back(pass);
}

bool DenoiseTask::load()
{
  string center_filepath = denoiser->input[frame];
  if (!image.load(center_filepath, error)) {
    return false;
  }

  /* Use previous frame output as input for subsequent frames. */
  if (frame > 0 && !image.load_previous(denoiser->output[frame - 1], error)) {
    return false;
  }

  if (image.layers.empty()) {
    error = "No image layers found to denoise in " + center_filepath;
    return false;
  }

  /* Enable temporal denoising for frames after the first (which will use the output from the
   * previous frames). */
  DenoiseParams params = denoiser->denoiser->get_params();
  params.temporally_stable = frame > 0;
  denoiser->denoiser->set_params(params);

  /* Allocate device buffer. */
  vector<Pass *> passes;
  add_pass(passes, PassType::PASS_COMBINED);
  add_pass(passes, PassType::PASS_DENOISING_ALBEDO);
  add_pass(passes, PassType::PASS_DENOISING_NORMAL);
  add_pass(passes, PassType::PASS_MOTION);
  add_pass(passes, PassType::PASS_DENOISING_PREVIOUS);
  add_pass(passes, PassType::PASS_COMBINED, PassMode::DENOISED);

  BufferParams buffer_params;
  buffer_params.width = image.width;
  buffer_params.height = image.height;
  buffer_params.full_x = 0;
  buffer_params.full_y = 0;
  buffer_params.full_width = image.width;
  buffer_params.full_height = image.height;
  buffer_params.update_passes(passes);

  for (Pass *pass : passes) {
    delete pass;
  }

  buffers.reset(buffer_params);

  /* Read pixels for first layer. */
  current_layer = 0;
  if (!load_input_pixels(current_layer)) {
    return false;
  }

  return true;
}

bool DenoiseTask::exec()
{
  for (current_layer = 0; current_layer < image.layers.size(); current_layer++) {
    /* Read pixels for secondary layers, first was already loaded. */
    if (current_layer > 0) {
      if (!load_input_pixels(current_layer)) {
        return false;
      }
    }

    /* Run task on device. */
    denoiser->denoiser->denoise_buffer(buffers.params, &buffers, 1, true);

    /* Copy denoised pixels from device. */
    buffers.buffer.copy_from_device();

    float *result = buffers.buffer.data(), *out = image.pixels.data();

    const DenoiseImageLayer &layer = image.layers[current_layer];
    const int *output_to_image_channel = layer.output_to_image_channel.data();

    for (int y = 0; y < image.height; y++) {
      for (int x = 0; x < image.width; x++, result += buffers.params.pass_stride) {
        for (int j = 0; j < OUTPUT_NUM_CHANNELS; j++) {
          int offset = buffers.params.get_pass_offset(PASS_COMBINED, PassMode::DENOISED);
          int image_channel = output_to_image_channel[j];
          out[image.num_channels * x + image_channel] = result[offset + j];
        }
      }
      out += image.num_channels * image.width;
    }

    printf("\n");
  }

  return true;
}

bool DenoiseTask::save()
{
  bool ok = image.save_output(denoiser->output[frame], error);
  free();
  return ok;
}

void DenoiseTask::free()
{
  image.free();
  buffers.buffer.free();
}

/* Denoise Image Storage */

DenoiseImage::DenoiseImage()
{
  width = 0;
  height = 0;
  num_channels = 0;
  samples = 0;
}

DenoiseImage::~DenoiseImage()
{
  free();
}

void DenoiseImage::close_input()
{
  in_previous.reset();
}

void DenoiseImage::free()
{
  close_input();
  pixels.clear();
}

bool DenoiseImage::parse_channels(const ImageSpec &in_spec, string &error)
{
  const std::vector<string> &channels = in_spec.channelnames;
  const ParamValue *multiview = in_spec.find_attribute("multiView");
  const bool multiview_channels = (multiview && multiview->type().basetype == TypeDesc::STRING &&
                                   multiview->type().arraylen >= 2);

  layers.clear();

  /* Loop over all the channels in the file, parse their name and sort them
   * by RenderLayer.
   * Channels that can't be parsed are directly passed through to the output. */
  map<string, DenoiseImageLayer> file_layers;
  for (int i = 0; i < channels.size(); i++) {
    string layer, pass, channel;
    if (parse_channel_name(channels[i], layer, pass, channel, multiview_channels)) {
      file_layers[layer].channels.push_back(pass + "." + channel);
      file_layers[layer].layer_to_image_channel.push_back(i);
    }
  }

  /* Loop over all detected RenderLayers, check whether they contain a full set of input channels.
   * Any channels that won't be processed internally are also passed through. */
  for (map<string, DenoiseImageLayer>::iterator i = file_layers.begin(); i != file_layers.end();
       ++i) {
    const string &name = i->first;
    DenoiseImageLayer &layer = i->second;

    /* Check for full pass set. */
    if (!layer.detect_denoising_channels()) {
      continue;
    }

    layer.name = name;
    layer.samples = samples;

    /* If the sample value isn't set yet, check if there is a layer-specific one in the input file.
     */
    if (layer.samples < 1) {
      string sample_string = in_spec.get_string_attribute("cycles." + name + ".samples", "");
      if (sample_string != "") {
        if (!sscanf(sample_string.c_str(), "%d", &layer.samples)) {
          error = "Failed to parse samples metadata: " + sample_string;
          return false;
        }
      }
    }

    if (layer.samples < 1) {
      error = string_printf(
          "No sample number specified in the file for layer %s or on the command line",
          name.c_str());
      return false;
    }

    layers.push_back(layer);
  }

  return true;
}

void DenoiseImage::read_pixels(const DenoiseImageLayer &layer,
                               const BufferParams &params,
                               float *input_pixels)
{
  /* Pixels from center file have already been loaded into pixels.
   * We copy a subset into the device input buffer with channels reshuffled. */
  const int *input_to_image_channel = layer.input_to_image_channel.data();

  for (int i = 0; i < width * height; i++) {
    for (int j = 0; j < 3; ++j) {
      int offset = params.get_pass_offset(PASS_COMBINED);
      int image_channel = input_to_image_channel[INPUT_NOISY_IMAGE + j];
      input_pixels[i * params.pass_stride + offset + j] =
          pixels[((size_t)i) * num_channels + image_channel];
    }
    for (int j = 0; j < 3; ++j) {
      int offset = params.get_pass_offset(PASS_DENOISING_NORMAL);
      int image_channel = input_to_image_channel[INPUT_DENOISING_NORMAL + j];
      input_pixels[i * params.pass_stride + offset + j] =
          pixels[((size_t)i) * num_channels + image_channel];
    }
    for (int j = 0; j < 3; ++j) {
      int offset = params.get_pass_offset(PASS_DENOISING_ALBEDO);
      int image_channel = input_to_image_channel[INPUT_DENOISING_ALBEDO + j];
      input_pixels[i * params.pass_stride + offset + j] =
          pixels[((size_t)i) * num_channels + image_channel];
    }
    for (int j = 0; j < 4; ++j) {
      int offset = params.get_pass_offset(PASS_MOTION);
      int image_channel = input_to_image_channel[INPUT_MOTION + j];
      input_pixels[i * params.pass_stride + offset + j] =
          pixels[((size_t)i) * num_channels + image_channel];
    }
  }
}

bool DenoiseImage::read_previous_pixels(const DenoiseImageLayer &layer,
                                        const BufferParams &params,
                                        float *input_pixels)
{
  /* Load pixels from neighboring frames, and copy them into device buffer
   * with channels reshuffled. */
  size_t num_pixels = (size_t)width * (size_t)height;
  array<float> neighbor_pixels(num_pixels * num_channels);
  if (!in_previous->read_image(TypeDesc::FLOAT, neighbor_pixels.data())) {
    return false;
  }

  const int *output_to_image_channel = layer.previous_output_to_image_channel.data();

  for (int i = 0; i < width * height; i++) {
    for (int j = 0; j < 3; ++j) {
      int offset = params.get_pass_offset(PASS_DENOISING_PREVIOUS);
      int image_channel = output_to_image_channel[j];
      input_pixels[i * params.pass_stride + offset + j] =
          neighbor_pixels[((size_t)i) * num_channels + image_channel];
    }
  }

  return true;
}

bool DenoiseImage::load(const string &in_filepath, string &error)
{
  if (!Filesystem::is_regular(in_filepath)) {
    error = "Couldn't find file: " + in_filepath;
    return false;
  }

  unique_ptr<ImageInput> in(ImageInput::open(in_filepath));
  if (!in) {
    error = "Couldn't open file: " + in_filepath;
    return false;
  }

  in_spec = in->spec();
  width = in_spec.width;
  height = in_spec.height;
  num_channels = in_spec.nchannels;

  if (!parse_channels(in_spec, error)) {
    return false;
  }

  if (layers.empty()) {
    error = "Could not find a render layer containing denoising data and motion vector passes";
    return false;
  }

  size_t num_pixels = (size_t)width * (size_t)height;
  pixels.resize(num_pixels * num_channels);

  /* Read all channels into buffer. Reading all channels at once is faster
   * than individually due to interleaved EXR channel storage. */
  if (!in->read_image(TypeDesc::FLOAT, pixels.data())) {
    error = "Failed to read image: " + in_filepath;
    return false;
  }

  return true;
}

bool DenoiseImage::load_previous(const string &filepath, string &error)
{
  if (!Filesystem::is_regular(filepath)) {
    error = "Couldn't find neighbor frame: " + filepath;
    return false;
  }

  unique_ptr<ImageInput> in_neighbor(ImageInput::open(filepath));
  if (!in_neighbor) {
    error = "Couldn't open neighbor frame: " + filepath;
    return false;
  }

  const ImageSpec &neighbor_spec = in_neighbor->spec();
  if (neighbor_spec.width != width || neighbor_spec.height != height) {
    error = "Neighbor frame has different dimensions: " + filepath;
    return false;
  }

  for (DenoiseImageLayer &layer : layers) {
    if (!layer.match_channels(in_spec.channelnames, neighbor_spec.channelnames)) {
      error = "Neighbor frame misses denoising data passes: " + filepath;
      return false;
    }
  }

  in_previous = std::move(in_neighbor);

  return true;
}

bool DenoiseImage::save_output(const string &out_filepath, string &error)
{
  /* Save image with identical dimensions, channels and metadata. */
  ImageSpec out_spec = in_spec;

  /* Ensure that the output frame contains sample information even if the input didn't. */
  for (int i = 0; i < layers.size(); i++) {
    string name = "cycles." + layers[i].name + ".samples";
    if (!out_spec.find_attribute(name, TypeDesc::STRING)) {
      out_spec.attribute(name, TypeDesc::STRING, string_printf("%d", layers[i].samples));
    }
  }

  /* We don't need input anymore at this point, and will possibly
   * overwrite the same file. */
  close_input();

  /* Write to temporary file path, so we denoise images in place and don't
   * risk destroying files when something goes wrong in file saving. */
  string extension = OIIO::Filesystem::extension(out_filepath);
  string unique_name = ".denoise-tmp-" + OIIO::Filesystem::unique_path();
  string tmp_filepath = out_filepath + unique_name + extension;
  unique_ptr<ImageOutput> out(ImageOutput::create(tmp_filepath));

  if (!out) {
    error = "Failed to open temporary file " + tmp_filepath + " for writing";
    return false;
  }

  /* Open temporary file and write image buffers. */
  if (!out->open(tmp_filepath, out_spec)) {
    error = "Failed to open file " + tmp_filepath + " for writing: " + out->geterror();
    return false;
  }

  bool ok = true;
  if (!out->write_image(TypeDesc::FLOAT, pixels.data())) {
    error = "Failed to write to file " + tmp_filepath + ": " + out->geterror();
    ok = false;
  }

  if (!out->close()) {
    error = "Failed to save to file " + tmp_filepath + ": " + out->geterror();
    ok = false;
  }

  out.reset();

  /* Copy temporary file to output filepath. */
  string rename_error;
  if (ok && !OIIO::Filesystem::rename(tmp_filepath, out_filepath, rename_error)) {
    error = "Failed to move denoised image to " + out_filepath + ": " + rename_error;
    ok = false;
  }

  if (!ok) {
    OIIO::Filesystem::remove(tmp_filepath);
  }

  return ok;
}

/* File pattern handling and outer loop over frames */

DenoiserPipeline::DenoiserPipeline(DeviceInfo &device_info, const DenoiseParams &params)
{
  /* Initialize task scheduler. */
  TaskScheduler::init();

  /* Initialize device. */
  device = Device::create(device_info, stats, profiler);
  device->load_kernels(KERNEL_FEATURE_DENOISING);

  denoiser = Denoiser::create(device, params);
  denoiser->load_kernels(nullptr);
}

DenoiserPipeline::~DenoiserPipeline()
{
  denoiser.reset();
  delete device;
  TaskScheduler::exit();
}

bool DenoiserPipeline::run()
{
  assert(input.size() == output.size());

  int num_frames = output.size();

  for (int frame = 0; frame < num_frames; frame++) {
    /* Skip empty output paths. */
    if (output[frame].empty()) {
      continue;
    }

    /* Execute task. */
    DenoiseTask task(device, this, frame);
    if (!task.load()) {
      error = task.error;
      return false;
    }

    if (!task.exec()) {
      error = task.error;
      return false;
    }

    if (!task.save()) {
      error = task.error;
      return false;
    }

    task.free();
  }

  return true;
}

CCL_NAMESPACE_END
