# SKP to Blender

이 프로젝트는 스케치업 파일(`.skp`)을 블렌더 파일(`.blend`)로 변환하기 위한 커스텀 블렌더 프로그램입니다.

프로젝트에 사용하는 블렌더 버전은 `v3.0.1` 버전입니다.

## Getting Started

아래의 모든 작업은 프로젝트 디렉터리를 기준으로 작성되었습니다.

### Install Programs

블렌더 프로그램을 설치하기 전에 필요한 프로그램을 설치합니다.

- CMake
- SVN
- (Windows) Visual Studio 2019 이상
  - Installer에서 **C++를 사용한 데스크톱 개발** 기본 옵션으로 함께 설치

### Build Blender

블렌더 프로그램 설치를 시작합니다.

```shell
git clone git@github.com:ACON3D/skp2blend-blender.git
cd skp2blend-blender

# Windows
./make.bat update 2019 (or 2022)
git submodule update --init --recursive --remote
./make.bat 2019 (or 2022)

# MacOS
make update
git submodule update --init --recursive --remote
make
```

### Run Blender

`make`를 끝내면 skp2blend-blender가 있는 디렉터리에 `build_xxx`이라는 폴더가 생성됩니다. 빌드 폴더 내부에 있는 블렌더 프로그램을 실행하면 됩니다.

- **Windows**

```shell
build_xxx/bin/Release/blender.exe
```

- **MacOS**

```shell
build_xxx/bin/Blender.app/Contents/MacOS/Blender
```

## Manage Packages

블렌더를 개발할 때 필요한 패키지는 `requirements.txt`에서 관리합니다.

주의할 점은 로컬에 설치된 Python을 사용해도 좋지만, 블렌더는 내장되어 있는 Python을 사용하기 때문에 이것을 사용하는 것이 훨씬 더 개발 정확도를 올릴 수 있습니다.

블렌더의 내장 Python 위치는 `build_xxx/bin/Release/{Blender Version}/python/bin`에 있습니다.

```shell
build_xxx/bin/Release/3.0/python/bin/python.exe -m pip install -r requirements.txt
```

## Using Converter

SKP Importer 사용 방법은 해당 저장소에 작성되어 있습니다.

### [🔗저장소 링크](https://github.com/ACON3D/skp2blend-importer)
