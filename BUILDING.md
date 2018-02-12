Building the CasparCG Server
============================

The CasparCG Server source code uses the CMake build system in order to easily
generate build systems for multiple platforms. CMake is basically a build
system for generating build systems.

On Windows we can use CMake to generate a .sln file and .vcproj files. On
Linux CMake can generate make files or ninja files. Qt Creator has support for
loading CMakeLists.txt files directly.

Windows
=======

Development using Visual Studio
-------------------------------

1. Install Visual Studio 2017.

2. Install CMake (http://www.cmake.org/download/).

3. `git clone --single-branch --branch 2.2.0 https://github.com/CasparCG/server casparcg-server-2.2.0`

4. `cd casparcg-server-2.2.0`

5. `cmake -G "Visual Studio 15 2017" -A x64 ./src`

6. Open `./src/CasparCG Server.sln`

Linux
=====

Building inside Docker
----------------------

1. `git clone --single-branch --branch 2.2.0 https://github.com/CasparCG/server casparcg-server-2.2.0`
2. `cd casparcg-server-2.2.0`
3. `tools/linux/build-in-docker`

If all goes to plan, a docker image has been created containing CasparCG Server.

Extracting CasparCG Server from Docker
--------------------------------------

1. `tools/linux/extract-from-docker`

You will then find a folder called `casparcg_server` which should contain everything you need to run CasparCG Server.

Development
-----------

1. Install dependencies `apt-get install .... (coming soon)`
2. Build boost and ffmpeg as per the docker images inside of `tools/linux` they should be saved in `/opt/boost` and `/opt/ffmpeg`
3. Download CEF from the link near the top of `tools/linux/Dockerfile`, and extract to `/opt/cef`
4. `git clone --single-branch --branch 2.2.0 https://github.com/CasparCG/server` casparcg-server-2.2.0
5. `mkdir casparcg-server-2.2.0-build && cd casparcg-server-2.2.0-build`
6. `cmake ../casparcg-server-2.2.0`
7. `make -j8`

If all goes to plan, a folder called 'staging' has been created with everything you need to run CasparCG server.
