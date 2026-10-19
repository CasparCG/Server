CasparCG 2.5.1 Stable
==========================================

### Core
##### Improvements
* Remove AVX2 requirement. It is still required for v210 decklink output, but not for other usage.
* Update FFmpeg to 8.1.2
##### Fixes
* AMCP: Prevent race when executing BATCH
* Linux: Support SFML 3 (not 100% feature parity)
* Windows: Prevent background throttling when no visible window
* Channel grid failing to initialise

### Producers
##### Fixes
* FFmpeg: Gracefully handle ENOMEM errorss
* FFmpeg: Reduce mutex contention
* FFmpeg: Ensure graph is stopped before joining
* HTML: Missing WebGPU libraries (windows only)

### Consumers
##### Improvements
* Decklink: Add option to disable keying
* Audio: Configurable delay
* Audio: Allow multiple devices to be used
* Audio: Improve sync to other consumers
* Screen: Configurable delay

##### Fixes
* Audio: Device enumeration issues
* Decklink: Recover av-sync in extreme cases
* Decklink: Check for keying support before enabling
* Decklink: Free model names string


CasparCG 2.5.0 Stable
==========================================

### Core
##### Improvements
* Initial support for HDR. This is limited to a subset of producers and consumers at this stage.
* Build for Windows with VS2022
* Rework linux builds to produce ubuntu deb files
* Update ffmpeg to 7.0
* Reimplement mixer transforms, to handle routes correctly
* Support more pixel formats from ffmpeg, to preserve colour accuracy better
* Support running on headless linux
* Transitions: Add FADECUT, CUTFADE and VFADE types, UP/DOWN directions for PUSH/SLIDE, and a STING cut mode
##### Fixes
* Build with boost 1.85/1.86/1.87/1.88
* Build with ffmpeg 7.1
* Only produce mixed frames on channels which have consumers
* Routed channels not compositing correctly when channel used a MIXER KEY
* Handle audio for fractional framerates properly
* Gracefully exit on SIGINT and SIGTERM

### Producers
##### Improvements
* FFmpeg: Support loading with a scaling-mode, to configure how clips get fit into the channel
* FFmpeg: Support more pixel formats without cpu conversion
* FFmpeg: Enable alpha for webm videos
* Image: Support loading with a scaling-mode, to configure how images get fit into the channel
* Image: Replace freeimage with ffmpeg
* HTML: Update CEF to 142
* HTML: Support audio
##### Fixes
* Route: Use full field rate when performing i->p channel route
* HTML: Gracefully handle page load errors
* HTML: Always set cache path

### Consumers
##### Improvements
* All: Support optional consumer ID via AMCP ADD/REMOVE commands and config.xml. Allows multiple consumers of the same type per channel and removal by ID.
  - AMCP: `ADD 1 NDI ID "my-id" NAME "Test"` / `REMOVE 1 ID "my-id"`
  - Config: `<ndi><id>my-id</id><name>Test</name></ndi>`
* Screen: Set size and position from AMCP
* Screen: Improve performance
* Image: Propagate AMCP parameters from PRINT command
* FFmpeg: Remove unnecessary forced conversion to YUVA422
* Decklink: Support explicit yuv output (requires AVX2)
* Decklink: Allow selecting device by hardware persistent id

##### Fixes
* FFmpeg: Correctly handle PTS on frame drop


CasparCG 2.4.3 Stable
==========================================

### Core
##### Fixes
* Improve error handling for invalid config files #1571
* Flush logs before exit #1571
* Check audio cadence values look sane before accepting format #1588
* Cross-channel routes from progressive to interlaced showing lots of black #1576
* Transition: ignoring some transforms of input frames #1602

### Producers
##### Fixes
* FFmpeg: fix crash on invalid frame header
* Decklink: Crash with ffmpeg 7 #1582
* HTML: Fix crash during uninit on exit
* Image: update state during init #1601

### Consumers
##### Fixes
* FFmpeg: set frame_rate for rtmp streams #1462


CasparCG 2.4.2 Stable
==========================================

### Consumers
##### Fixes
* Decklink: fix support for driver 14.3 and later


CasparCG 2.4.1 Stable
==========================================

### Core
##### Fixes
* Fix bad config file examples
* Fix `casparcg_auto_restart.bat` not starting scanner
* Revert removal of tbbmalloc, due to notable performance loss on windows
* Suppress some cmake build warnings
* Build failure when doxygen installed on system
* Build failures with ffmpeg 7.0
* Revert RPATH linking changes

### Producers
##### Fixes
* FFmpeg: Ignore ndi:// urls
* FFmpeg: Using both in and seek could result in incorrect duration
* Route: Race condition during destruction
* Image: Update freeimage on windows with some CVE fixes and failures with certain pngs
* Image: Respect EXIF rotate flag
* NDI: list local sources

### Consumers
##### Fixes
* Decklink: subregion copy not respecting frame height
* Decklink: subregion vertical offset
* Decklink: subregion height limited with some formats


CasparCG 2.4.0 Stable
==========================================

### Core
##### Improvements
* Custom resolutions can be specified in casparcg.config
* Interlaced mixer pipeline to ensure field accuracy
* Preserve unicode characters in console input/output
* Producers to be run at startup can be defined in casparcg.config
* Support 8K frames
* Support 4K DCI frames
* Remove undocumented CII and CLK protocol implementations
* Config parameter can be an absolute system path, not just relative to the working directory
* AMCP: Add CLEAR ALL command
* AMCP: Command batching syntax
* AMCP: LOAD/LOADBG/PLAY commands accept a CLEAR_ON_404 parameter, to instruct the layer to be cleared when the requested file was not found
* AMCP: Add commands to subscribe and unsubscribe to OSC on any port number
* AMCP: Add CALLBG command to perform CALL on background producer
* Build: Require C++17 for building
* Build: Support newer versions of Boost
* Build: Support newer versions of TBB
* Build: Disable precompiled headers for linux
* Build: Support VS2022
* Build: Replace nuget and locally committed dependencies with direct http downloads
* Build: Allow configuring diag font path at build time
* Linux: Support setting thread priorities
* Linux: Initial ARM64 compatibility
* Linux: Rework build to always use system boost
* Linux: Rework build process to better support being build as a system package
* Logging: add config option to disable logging to file and to disable column alignment
* Transitions: Support additional audio fade properties for STING transition
##### Fixes
* Crash upon exiting if HTML producer was running
* AMCP: Ensure all consumers and producers are reported in `INFO` commands
* AMCP: Deferred mixer operations were not being cleared after being applied
* AMCP: `LOAD` command would show a frame or two of black while new producer was loading
* OpenGL: Fix support for recent Linux drivers
* Linux: Fix endless looping on stdin
* Route: Fix error when clearing layer
* Transitions: Fix wipe duration

### Producers
##### Improvements
* Decklink: Require driver 11.0 or later
* Decklink: Scale received frames on GPU
* FFmpeg: Update to v5.1
* FFmpeg: Improve performance
* FFmpeg: Allow specifying both SEEK and IN for PLAY commands
* HTML: Update to CEF 117
* HTML: `CALL 1-10 RELOAD` to reload a renderer
* HTML: Expose `cache-path` setting
* NDI: Upgrade to NDI5
* System Audio: Allow specifying output device to use
##### Fixes
* Decklink: Log spamming when using some input formats
* FFmpeg: Prevent loading unreadable files
* FFmpeg: Unable to play files with unicode filenames
* FFmpeg: Don't lowercase filter parameters
* FFmpeg: Support parameters with name containing a dash
* HTML: media-stream permission denied
* HTML: Expose angle backend config field, the best backend varies depending on the templates and machine
* HTML: Crash when multiple iframes were loaded within a renderer
* Image: Improve file loading algorithm to match the case insensitive and absolute path support already used by ffmpeg

### Consumers
##### Improvements
* Artnet: New artnet consumer
* Decklink: Configure device duplex modes in casparcg.config
* Decklink: Output a subregion of the channel
* Decklink: Add secondary outputs in a consumer, to ensure sync when used within a single card
* iVGA: Remove consumer
* NDI: Upgrade to NDI5
##### Fixes
* Decklink: Fix stutter when loading clips
* FFmpeg: Fix RTMP streaming missing headers
* NDI: dejitter


CasparCG 2.3.3 LTS Stable
==========================================

### Producers
##### Improvements
* Image Scroll Producer: Ported from 2.1


CasparCG 2.3.2 LTS Stable
==========================================

### Producers
##### Fixes
* Packages: Update TBB library to v2021.1.1 - fixes CPU and memory growth when deleting threads
* FFmpeg: Fix possible deadlock leading to producer not being cleaned up correctly


CasparCG 2.3.2 Beta
==========================================

### Producers
##### Fixes
* Packages: Update TBB library to v2021.1.1 - fixes CPU and memory growth when deleting threads
* FFmpeg: Fix possible deadlock leading to producer not being cleaned up correctly


CasparCG 2.3.1 Stable
==========================================

### Producers
##### Fixes
* Flash: Use proper file urls when loading templates, to allow it to work after Flash Player EOL
* FFmpeg: Various HTTP playback improvements


CasparCG 2.3.0 Stable
==========================================

### Producers
##### Features
* FFmpeg: Add more common file extensions to the supported list
* NDI: Require minimum of NDI v4.0
##### Fixes
* HTML: Minimise performance impact on other producers


CasparCG 2.3.0 RC
==========================================

### Producers
##### Features
* Flash: Disable by default, requires enabling in the config file
* FFmpeg: Remove fixed thread limit to better auto select a number
##### Fixes
* Decklink: Downgrade severity of video-format not supported
* FFmpeg: Correctly handle error codes. Ignore exit errors during initialisation
* Route: Detect circular routes and break the loop

### Consumers
##### Features
* Bluefish: Various improvmements including support for Kronos K8

### General
##### Fixes
* Diag not reflecting channel videoformat changes


CasparCG 2.3.0 Beta 1
==========================================

### Producers
##### Features
* Decklink: Detect and update input format when no format is specified in AMCP
* Decklink: Improve performance (gpu colour conversion & less heavy deinterlacing when possible)
* Decklink: `LOAD DECKLINK` will display live frames instead of black
* FFmpeg: Update to 4.2.2
* HTML: Better performance for gpu-enabled mode
* HTML: `window.remove()` has been partially reimplemented
* NDI: Native NDI producer
* Route: Allow routing first frame of background producer
* Route: zero delay routes when within a channel, with 1 frame when cross-channel
* Transition: Add sting transitions
* Add frames_left field to osc/info for progress towards autonext
##### Fixes
* Colour: parsing too much of amcp string as list of colours
* FFmpeg: Always resample clips to 48khz
* FFmpeg: Ensure frame time reaches the end of the clip
* FFmpeg: RTMP stream playback
* FFmpeg: SEEK and LENGTH parameters causing issues with AUTONEXT
* FFmpeg: Ensure packets/frames after the decided end of the clip are not displayed
* FFmpeg: Incorrect seek for audio when not 48khz
* FFmpeg: Some cases where it would not be destroyed if playing a bad stream
* HTML: unlikely but possible exception when handling frames
* HTML: set autoplay-policy
* HTML: animations being ticked too much
* Route: Sending empty frame into a route would cause the destination to reuse the last frame

### Consumers
##### Features
* Audio: Fix audio crackling
* Audio: Fix memory leak
* Bluefish: Various improvmements including supporting more channels and UHD.
* NDI: Native NDI consumer
* Screen: Add side by side key output
* Screen: Add support for Datavideo TC-100/TC-200
##### Fixes
* Decklink: Tick channel at roughly consistent rate when running interlaced output
* Possible crash when adding/removing consumers

### General
##### Features
* Add mixer colour invert property
* Restore `INFO CONFIG` and `INFO PATHS` commands
* Linux: Update docker images to support running in docker (not recommended for production use)
##### Fixes
* NTSC audio cadence
* Ignore empty lines in console input
* Fix building with clang on linux
* Fix building with vs2019
* Better error when startup fails due to AMCP port being in use
* Backslash is a valid trailing slash for windows

CasparCG 2.2.0
==========================================

General
-------

 * C++14
 * Major refactoring, cleanup, optimization
   and stability improvements.
 * Removed unmaintained documentation API.
 * Removed unmaintained program options API.
 * Removed unused frame age API.
 * Removed misc unused and/or unmaintained APIs.
 * Removed TCP logger.
 * Fixed memory leak in transition producer.
 * Removed PSD Producer (moved to 3.0.0).
 * Removed Text Producer (moved to 3.0.0).
 * Removed SyncTo consumer.
 * Removed channel layout in favor of 8 channel passthrough
    and FFMPEG audio filters.
 * Major stability and performance improvements of GPU code.
 * Requires OpenGL 4.5.
 * Repo cleanup (>2GB => <100MB when cloning).
 * Misc cleanup and fixes.

Build
-----
 * Linux build re done with Docker.
 * Windows build re done with Nuget.

HTML
----
 * Updated to Chromium 63 (Julusian).
 * Allow running templates from arbitrary urls (Julusian).

DECKLINK
--------
 * Fixed broken Linux.
 * Misc cleanup and fixes.
 * Complex FFMPEG filters (VF, AF).

MIXER
-----
 * Performance improvements.
 * Removed straight output (moved to 3.0.0).
 * Proper OpenGL pipelining.
 * Blend modes are always enabled.
 * Misc cleanup and fixes.
 * Removed CPU mixer.
 * Mixer always runs in progressive mode. Consumers are expected to convert to interlaced if required.

IMAGE
-----
 * Correctly apply alpha to base64 encoded pngs from AMCP (Julusian).
 * Unmultiply frame before writing to png (Julusian).
 * Removed scroll producer (moved to 3.0.0)

 ROUTE
 -----

 * Reimplemented, simplified.
 * Cross channel routing will render full stage instead of simply copying channel output.
 * Reduced overhead and latency.

FFMPEG
------
 * Rewritten from scratch for better accuracy, stability and
    performance.
 * Update freezed frame during seeking.
 * FFMPEG 3.4.1.
 * Reduce blocking during initialization.
 * Fixed timestamp handling.
 * Fixed V/A sync.
 * Fixed interlacing.
 * Fixed framerate handling.
 * Fixed looping.
 * Fixed seeking.
 * Fixed duration.
 * Audio resampling to match timestamps.
 * Fixed invalid interlaced YUV (411, 420) handling.
 * Added YUV(A)444.
 * Added IO timeout.
 * Added HTTP reconnect.
 * FFMPEG video filter support.
 * FFMPEG audio filter support.
 * Complex FFMPEG filters (VF, AF).
 * CALL SEEK return actually sought value.
 * All AMCP options are based on channel format.
 * Misc improvements, cleanup and fixes.

Bluefish
--------
 * Misc cleanup and fixes.

OAL
------------
 * Added audio sample compensation to avoid audio distortions
    during time drift.
 * Misc cleanup and fixes.

Screen
---------------
 * Proper OpenGL pipelining.
 * Misc cleanup and fixes.

AMCP
----
 * Added PING command (Julusian).
 * Removed INFO commands in favor of OSC.
 * Moved CLS, CINF, TLS, FLS, TLS, THUMBNAIL implementations into
    a separate NodeJS service which is proxied through
    an HTTP API.
 * Misc cleanup and fixes.

CasparCG 2.1.0 Next (w.r.t 2.1.0 Beta 2)
==========================================

General
-------

 * Removed asmlib dependency in favor of using standard library std::memcpy and
    std::memset, because of better performance.

CasparCG 2.1.0 Beta 2 (w.r.t 2.1.0 Beta 1)
==========================================

General
-------

 * Fail early with clear error message if configured paths are not
    creatable/writable.
 * Added backwards compatibility (with deprecation warning) for using
    thumbnails-path instead of thumbnail-path in casparcg.config.
 * Suppress the logging of full path names in stack traces so that only the
    relative path within the source tree is visible.
 * General stability improvements.
 * Native thread id is now logged in Linux as well. Finally they are mappable
    against INFO THREADS, ps and top.
 * Created automatically generated build number, so that it is easier to see
    whether a build is newer or older than an other.
 * Changed configuration element mipmapping_default_on to mipmapping-default-on
    for consistency with the rest of the configuration (Jesper Stærkær).
 * Handle stdin EOF as EXIT.
 * Added support for RESTART in Linux startup script run.sh.
 * Copy casparcg_auto_restart.bat into Windows releases.
 * Fixed bug with thumbnail generation when there are .-files in the media
    folder.
 * Removed CMake platform specification in Linux build script
    (Krzysztof Pyrkosz).
 * Build script for building FFmpeg for Linux now part of the repository.
    Contributions during development (not w.r.t 2.1.0 Beta 1):
   * Fix ffmpeg build dependencies on clean Ubuntu desktop amd64 14.04.3 or
      higher (Walter Sonius).
 * Added support for video modes 2160p5000, 2160p5994 and 2160p6000
    (Antonio Ruano Cuesta).
 * Fixed serious buffer overrun in FFmpeg logging code.

Consumers
---------

 * FFmpeg consumer:
   * Fixed long overdue bug where HD material was always recorded using the
      BT.601 color matrix instead of the BT.709 color matrix. RGB codecs like
      qtrle was never affected but all the YCbCr based codecs were.
   * Fixed bug in parsing of paths containing -.
   * Fixed bugs where previously effective arguments like -pix_fmt were
      ignored.
   * Fixed bug where interlaced channels where not recorded correctly for
      some codecs.
 * DeckLink consumer:
   * Rewrote the frame hand-off between send() and ScheduledFrameCompleted() in
      a way that hopefully resolves all dead-lock scenarios previously possible.
 * Bluefish consumer:
   * Largely rewritten against newest SDK Driver 5.11.0.47 (Satchit Nambiar and
      James Wise sponsored by Bluefish444):
     * Added support for Epoch Neutron and Supernova CG. All current Epoch
        cards are now supported.
     * Added support for for multiple SDI channels per card. 1 to 4 channels
        per Bluefish444 card depending on model and firmware.
     * Added support for single SDI output, complementing existing external key
        output support.
     * Added support for internal key using the Bluefish444 hardware keyer.
 * Screen consumer:
   * Fixed full screen mode.

Producers
---------

 * FFmpeg producer:
   * Increased the max number of frames that audio/video can be badly
      interleaved with (Dimitry Ishenko).
   * Fixed bug where decoders sometimes requires more than one video packet to
      decode the first frame.
   * Added support for IN and OUT parameters (Dimitry Ishenko).
   * Added DV/HDV video device support under Linux (Walter Sonius).
   * Remove unused flags variable in queued_seek (Dimitry Ishenko).
   * Now recognizes .ts files without probing contents (Ovidijus Striaukas).
   * Fixed uninitialized value causing initial log printout to usually say that
      clips are interlaced when they are not.
 * Destroy producer proxy:
   * Created workaround for bug in FFmpeg where every new thread used to
      cleanup caused handles to leak (not sure why). Reduced the effect by using
      only one thread for all producer destructions.
 * Framerate producer:
   * Fixed bug when INFO was used on a not yet playing framerate producer.
 * HTML producer:
   * Fixed bug where only URL:s with . in them where recognized.
 * Image producer:
   * Added LENGTH parameter to allow for queueing with LOADBG AUTO.
   * Fixed inconsistency in what file extensions are supported vs listed in
      CLS/CINF.
 * Layer producer:
   * Fixed serious bug where a circular reference of layer producers caused a
      stack overflow and server crash.
   * Can now route from layer on a channel with an incompatible framerate.
 * Channel producer:
   * Can now route from channel with an incompatible framerate.
   * Deinterlaces interlaced content from source channel.
   * Added optional NO_AUTO_DEINTERLACE parameter to opt out of the mentioned
      deinterlacing.
 * Scene producer:
   * Added abs(), floor(), to_lower(), to_upper() and length() functions to the
      expression language.
   * Created XML Schema for the *.scene XML format. Allows for IDE-like auto-
      completion, API documentation and validation.
   * Added possibility to specify the width and height of a layer instead of
      letting the producer on the layer decide.
   * Added global variables scene_width, scene_height and fps.
   * Made it possible to use expressions in keyframe values.
   * Fixed serious bug where uninitialized values were used.
   * Created more example scenes.
   * Can now forward CALL, CG PLAY, CG STOP, CG NEXT and CG INVOKE to the
      producer on a layer.
 * CG proxy wrapper producer:
   * New in 2.1.0.
   * Allows all CG producers to be used as an ordinary producer inside a layer
      in a scene.
   * Allows the Scene producer to know what variables are available in a
      template.
 * Color producer:
   * Now has support for gradients.
 * PSD producer:
   * Added support for centered and right justified text.
 * Text producer:
   * Fixed bug where tracking contributed to the overall text width on the
      last character.

Mixer
-----

 * Fixed bug in the contrast/saturation/brightness code where the wrong luma
    coefficients was used.
 * Rewrote the chroma key code to support variable hue, instead of fixed green
    or blue. Threshold setting was removed in favour of separate hue width,
    minimum saturation and minimum brightness constraints. Also a much more
    effective spill suppression method was implemented.
 * Fixed bug where glReadPixels() was done from the last drawn to texture
    instead of always from the target texture. This means that for example a
    MIXER KEYER layer without a layer above to key, as well as a separate alpha
    file with MIXER OPACITY 0 now works as expected.
 * Fixed bug where already drawn GL_QUADS were not composited against, causing
    for example italic texts to be rendered incorrectly in the text_producer.

AMCP
----

 * INFO PATHS now adds all the path elements even if they are using the default
    values.
 * MIXER CHROMA syntax deprecated (still supported) in favour of the more
    advanced syntax required by the rewritten chroma key code.
 * Added special command REQ that can be prepended before any command to
    identify the response with a client specified request id, allowing a client
    to know exactly what asynchronous response matched a specific request.
 * Added support for listing contents of a specific directory for CLS, TLS,
    DATA LIST and THUMBNAIL LIST.
 * Fixed bug where CINF only returned the first match.
 * Fixed bug where a client closing the connection after BYE instead of
    letting the server close the connection caused an exception to be logged.



CasparCG 2.1.0 Beta 1 (w.r.t 2.0.7 Stable)
==========================================

General
-------

 * 64 bit!
 * Linux support!
   * Moved to CMake build system for better platform independence.
     * Contributions before build system switch (not w.r.t 2.0.7 Stable):
       * gitrev.bat adaptions for 2.1 (Thomas Kaltz III).
   * Thanks to our already heavy use of the pimpl idiom, abstracting platform
      specifics was easily done by having different versions of the .cpp files
      included in the build depending on target platform. No #ifdef necessary,
      except for in header only platform specific code.
   * Flash, Bluefish and NewTek modules are not ported to the Linux build.
   * Contributions during development (not w.r.t 2.0.7 Stable):
     * Fixed compilation problems in Linux build (Dimitry Ishenko).
     * Fixed compilation problem in GCC 5 (Krzysztof Pyrkosz).
     * Fixed thumbnail image saving on Linux (Krzysztof Pyrkosz).
     * Fixed compilation problem in PSD module (Krzysztof Pyrkosz).
 * Major code refactoring:
   * Mixer abstraction so different implementations can be created. Currently
      CPU mixer and GPU mixer (previously the usage of the GPU was mandatory)
      exists.
   * Flattened folder structure for easier inclusion of header files.
   * Many classes renamed to better describe the abstractions they provide.
   * Sink parameters usually taken by value and moved into place instead of
      taken by const reference as previously done.
   * Old Windows specific AsyncEventServer class has been replaced by platform
      independent implementation based on Boost.Asio.
   * Pimpl classes are now stack allocated with internal shared_ptr to
      implementation, instead of both handle and body being dynamically
      allocated. This means that objects are now often passed by value instead
      of via safe_ptr/shared_ptr, because they are internally reference counted.
   * Protocol strategies are now easier to implement correctly, because of
      separation of state between different client connections.
   * Complete AMCP command refactoring.
   * On-line help system that forces the developer to document AMCP commands,
      producer syntaxes and consumer syntaxes making the documentation coupled
      to the code, which is great.
     * Added missing help for VERSION command (Jesper Stærkær).
   * Upgraded Windows build to target Visual Studio 2015 making it possible to
      use the C++11 features also supported by GCC 4.8 which is targeted on
      Linux.
     * Fixed compilation problems in Visual Studio 2015 Update 1
        (Roman Tarasov)
   * Created abstraction of the different forms of templates (flash, html, psd
      and scene). Each module registers itself as a CG producer provides. All CG
      commands transparently works with all of them.
   * Audio mixer now uses double samples instead of float samples to fully
      accommodate all int32 samples.
   * Reduced coupling between core and modules (and modules and modules):
     * Modules can register system info providers to contribute to INFO SYSTEM.
     * XML configuration factories for adding support for new consumer elements
        in casparcg.config.
     * Server startup hooks can be registered (used by HTML producer to fork
        its sub process).
     * Version providers can contribute content to the VERSION command.
 * Refactored multichannel audio support to use FFmpeg's PAN filter and
    simplified the configuration a lot.
 * Upgraded most third party libraries we depend on.
 * Some unit tests have been created.
 * Renamed README.txt to README, CHANGES.txt to CHANGELOG and LICENSE.txt to
    LICENSE
 * Created README.md for github front page in addition to README which is
    distributed with builds.
 * README file updates (Jonas Hummelstrand).
 * Created BUILDING file describing how to build the server on Windows and
    Linux.
 * Diagnostics:
   * Now also sent over OSC.
   * Diag window is now scrollable and without squeezing of graphs.
   * Contextual information such as video channel and video layer now included
      in graphs.
 * Logging:
   * Implemented a TCP server, simply sending every log line to each connected
      client. Default port is 3250.
   * Changed default log level to info and moved debug statements that are
      interesting in a production system to info.
   * Try to not log full stack traces when user error is the cause. Stacktraces
      should ideally only be logged when a system error or a programming error
      has occurred.
   * More contextual information about an error added to exceptions. An example
      of this is that XML configuration errors now cause the XPath of the error
      is logged.
   * Improved the readability of the log format.
   * Added optional calltrace.log for logging method calls. Allows for trace
      logging to be enabled while calltracing is disabled etc.

OSC
---

 * Improved message formatting performance.
 * Added possibility to disable sending OSC to connected AMCP clients.
 * Fixed inconsistent element name predefined_client to predefined-client in
    casparcg.config (Krzysztof Pyrkosz).

Consumers
---------

 * System audio consumer:
   * Pushes data to openal instead of being callbacked by SFML when data is
      needed.
   * Added possibility to specify the expected delay in the sound card. Might
      help get better consumer synchronization.
 * Screen consumer:
   * Added mouse interaction support, usable by the producers running on the
      video channel.
 * FFmpeg consumer:
   * Replaced by Streaming Consumer after it was adapted to support everything
      that FFmpeg Consumer did.
   * Added support for recording all audio channels into separate mono audio
      streams.
   * Now sends recording progress via OSC.
 * SyncTo consumer:
   * New in 2.1.0.
   * Allows the pace of a channel to follow another channel. This is useful for
      virtual "precomp" channels without a DeckLink consumer to pace it.
 * DeckLink consumer:
   * Added workaround for timescale bug found in Decklink SDK 10.7.
   * Now ScheduledFrameCompleted is no longer only used for video scheduling
      but for audio as well, simplifying the code a lot.
 * iVGA consumer:
   * No longer provides sync to the video channel.
   * Supports NewTek NDI out of the box just by upgrading the
      Processing.AirSend library.

Producers
---------

 * Scene producer:
   * New in 2.1.0.
   * Utilizes CasparCG concepts such as producers, mixer transforms and uses
      them in a nested way to form infinite number of sub layers. Think movie
      clip in Flash.
   * A scene consists of variables, layers, timelines and marks (intro and
      outro for example).
   * Mostly for use by other producers but comes with a XML based producer that
      is a registered CG producer and shows up in TLS.
   * Enables frame accurate compositions and animations.
   * Has a powerful variable binding system (think expressions in After Effects
      or JavaFX Bindings).
 * PSD producer:
   * New in 2.1.0.
   * Parses PSD files and sets up a scene for the Scene producer to display.
   * Text layers based on CG parameters.
   * Supports Photoshop timeline.
   * Uses Photoshop comment key-frames to describe where intro and outro (CG
      PLAY and CG STOP) should be in the timeline.
   * Shows up as regular templates in TLS.
 * Text producer:
   * New in 2.1.0.
   * Renders text using FreeType library.
   * Is used by the PSD producer for dynamic text layers.
 * Image scroll producer:
   * Speed can be changed while running using a CALL. The speed change can be
      tweened.
   * Added support for an absolute end time so that the duration is calculated
      based on when PLAY is called for shows when an exact end time is
      important.
 * Image producer:
   * Fixed bug where too large (OpenGL limit) images were accepted, causing
      problems during thumbnail generation.
 * Framerate producer:
   * New in 2.1.0.
   * Wraps a producer with one framerate and converts it to another. It is not
      usable on its own but is utilized in the FFmpeg producer and the DeckLink
      consumer.
   * Supports different interpolation algorithms. Currently a no-op
      drop-and-repeat mode and a two different frame blending modes.
   * It also supports changing the speed on demand with tweening support.
 * FFmpeg producer:
   * Supports decoding all audio streams from a clip. Useful with .mxf files
      which usually have separate mono streams for every audio channel.
   * No longer do framerate conversion (half or double), but delegates that
      task to the Framerate producer.
   * Added support for v4l2 devices.
   * Added relative and "from end" seeking (Dimitry Ishenko).
   * Contributions during development (not w.r.t 2.0.7 Stable):
     * Fixed 100% CPU problem on clip EOF (Peter Keuter, Robert Nagy).
     * Constrained SEEK within the length of a clip (Dimitry Ishenko).
     * Fixed a regular expression (Dimitry Ishenko).
 * DeckLink producer:
   * No longer do framerate conversion (half or double), but delegates that
      task to the Framerate producer.
 * Route producer:
   * Added possibility to delay frames routed from a layer or a channel.
 * HTML Producer:
   * Disabled web security in HTML Producer (Robert Nagy).
   * Reimplemented requestAnimationFrame handling in Javascript instead of C++.
   * Implemented cancelAnimationFrame.
   * Increased animation smoothness in HTML Producer with interlaced video
      modes.
   * Added remote debugging support.
   * Added mouse interaction support by utilizing the Screen consumer's new
      interaction support.
 * Flash Producer:
   * Contributions during development (not w.r.t 2.0.7 Stable):
     * Workaround for flickering with high CPU usage and CPU accelerator
        (Robert Nagy)

AMCP
----

 * TLS has a new column for "template type" for clients that want to
    differentiate between html and flash for example.
 * SET CHANNEL_LAYOUT added to be able to change the audio channel layout of a
    video channel at runtime.
 * HELP command added for accessing the new on-line help system.
 * FLS added to list the fonts usable by the Text producer.
 * LOCK command added for controlling/gaining exclusive access to a video
    channel.
 * LOG CATEGORY command added to enable/disable the new log categories.
 * SWAP command now optionally supports swapping the transforms as well as the
    layers.
 * VERSION command can now provide CEF version.



CasparCG Server 2.0.7 Stable (as compared to CasparCG Server 2.0.7 Beta 2)
==========================================================================

General
-------

 * Added support for using a different configuration file at startup than the
    default casparcg.config by simply adding the name of the file to use as the
    first command line argument to casparcg.exe.
 * Upgraded FFmpeg to latest stable.
 * Created build script.
 * Fixed bug where both layer_producer and channel_producer display:s and
    empty/late first frame when the producer is called before the consumer in
    the other end has received the first frame.
 * Added rudimentary support for audio for layer_producer and channel_producer.
 * Upgraded DeckLink SDK to 10.1.4, bringing new 2K and 4K DCI video modes. New
    template hosts also available for those modes.
 * General bug fixes (mostly memory and resource leaks, some serious).
 * Updated Boost to version 1.57
 * Frontend no longer maintained and therefore not included in the release.

Mixer
-----

 * Added support for rotation.
 * Added support for changing the anchor point around which fill_translation,
    fill_scale and rotation will be done from.
 * Added support for perspective correct corner pinning.
 * Added support for mipmapped textures with anisotropic filtering for
    increased downscaling quality. Whether to enable by default can be
    configured in casparcg.config.
 * Added support for cropping a layer. Not the same as clipping.

AMCP
----

 * Added RESUME command to complement PAUSE. (Peter Keuter)
 * To support the new mixer features the following commands has been added:

   * MIXER ANCHOR -- will return or modify the anchor point for a layer
      (default is 0 0 for backwards compatibility). Example:
      MIXER 1-10 ANCHOR 0.5 0.5
      ...for changing the anchor to the middle of the layer
      (a MIXER 1-10 FILL 0.5 0.5 1 1 will be necessary to place the layer at the
      same place on screen as it was before).

   * MIXER ROTATION -- will return or modify the angle of which a layer is
      rotated by (clockwise degrees) around the point specified by ANCHOR.

   * MIXER PERSPECTIVE -- will return or modify the corners of the perspective
      transformation of a layer. One X Y pair for each corner (order upper left,
      upper right, lower right and lower left). Example:
      MIXER 1-10 PERSPECTIVE 0.4 0.4 0.6 0.4 1 1 0 1

   * MIXER MIPMAP -- will return or modify whether to enable mipmapping of
      textures produced on a layer. Only frames produced after a change will be
      affected. So for example image_producer will not be affected while the
      image is displayed.

   * MIXER CROP -- will return or modify how textures on a layer will be
      cropped. One X Y pair each for the upper left corner and for the lower
      right corner.

 * Added INFO QUEUES command for debugging AMCP command queues. Useful for
    debugging command queue overflows, where a command is deadlocked. Hopefully
    always accessible via console, even though the TCP command queue may be
    full.
 * Added GL command:
    - GL INFO prints information about device buffers and host buffers.
    - GL GC garbage collects pooled but unused GL resources.
 * Added INFO THREADS command listing the known threads and their descriptive
    names. Can be matched against the thread id column of log entries.

Consumers
---------

 * Removed blocking_decklink_consumer. It was more like an experiment at best
    and its usefulness was questionable.
 * Added a 10 second time-out for consumer sends, to detect/recover from
    blocked consumers.
 * Some consumers which are usually added and removed during playout (for
    example ffmpeg_consumer, streaming_consumer and channel_consumer) no longer
    affect the presentation time on other consumers. Previously a lag on the SDI
    output could be seen when adding such consumers.

HTML producer
-------------

 * No longer tries to play all files with a . in their name.
    (Georgi Chorbadzhiyski)
 * Reimplemented using CEF3 instead of Berkelium, which enables use of WebGL
    and more. CEF3 is actively maintained, which Berkelium is not. (Robert Nagy)
 * Implements a custom version of window.requestAnimationFrame which will
    follow the pace of the channel, for perfectly smooth animations.
 * No longer manually interlaces frames, to allow for mixer fill transforms
    without artifacts.
 * Now uses CEF3 event loop to avoid 100% CPU core usage.



CasparCG Server 2.0.7 Beta 2 (as compared to CasparCG Server 2.0.7 Beta 1)
==========================================================================

General
-------

 * Added sending of OSC messages for channel_grid channel in addition to
    regular channels.

Producers
---------

 * FFmpeg: Reports correct nb_frames() when using SEEK (Thomas Kaltz III)
 * Flash: Fixed bug where CG PLAY, CG INVOKE did not work.

Consumers
---------

 * channel_consumer: Added support for more than one channel_consumer per
    channel.
 * decklink_consumer: Added support for a single instance of the consumer to
    manage a separate key output for use with DeckLink Duo/Quad cards:

    <decklink>
      <device>1</device>
      <key-device>2</key-device>
      <keyer>external_separate_device</keyer>
    </decklink>

    ...in the configuration will enable the feature. The value of <key-device />
    defaults to the value of <device /> + 1.
 * synchronizing_consumer: Removed in favour of a single decklink_consumer
    managing both fill and key device.
 * streaming_consumer: A new implementation of ffmpeg_consumer with added
    support for streaming and other PTS dependent protocols. Examples:

    <stream>
      <path>udp://localhost:5004</path>
      <args>-vcodec libx264 -tune zerolatency -preset ultrafast -crf 25 -format mpegts -vf scale=240:180</args>
    </stream>

    ...in configuration or:

    ADD 1 STREAM udp://localhost:5004 -vcodec libx264 -tune zerolatency -preset ultrafast -crf 25 -format mpegts -vf scale=240:180

    ...via AMCP. (Robert Nagy sponsored by Ericsson Broadcasting Services)
 * newtek_ivga_consumer: Added support for iVGA consumer to not provide channel
    sync even though connected. Useful for iVGA clients that downloads as fast
    as possible instead of in frame-rate pace, like Wirecast. To enable:

    <newtek-ivga>
      <provide-sync>false</provide-sync>
    </newtek-ivga>

    ...in config to not provide channel sync when connected. The default is
    true.

AMCP
----

 * Added support in ADD and REMOVE for a placeholder <CLIENT_IP_ADDRESS> which
    will resolve to the connected AMCP client's IPV4 address.
 * Fixed bug where AMCP commands split into multiple TCP packets where not
    correctly parsed (http://casparcg.com/forum/viewtopic.php?f=3&t=2480)



CasparCG Server 2.0.7 Beta 1 (as compared to 2.0.6 Stable)
==========================================================

General
-------
 * FFmpeg: Upgraded to master and adapted CasparCG to FFmpeg API changes
    (Robert Nagy sponsored by SVT)
 * FFmpeg: Fixed problem with frame count calculation (Thomas Kaltz III)
 * Fixed broken CG UPDATE.

Producers
---------

 * New HTML producer has been created (Robert Nagy sponsored by Flemish Radio
    and Television Broadcasting Organization, VRT)



CasparCG Server 2.0.6 Stable (as compared to 2.0.4 Stable)
==========================================================

General
-------
 * iVGA: Allow for the server to work without Processing.AirSend.x86.dll to
    prevent a possible GPL violation. It is available as a separate optional
    download.
 * iVGA: Only provide sync to channel while connected, to prevent channel
    ticking too fast.
 * FFmpeg: Fixed bug during deinterlace-bob-reinterlace where output fields
    were offset by one field in relation to input fields.
 * FFmpeg: Fixed bug in ffmpeg_consumer where an access violation occurred
    during destruction.
 * FFmpeg: Improved seeking. (Robert Nagy and Thomas Kaltz III)
 * Frontend: Only writes elements to casparcg.config which overrides a default
    value to keep the file as compact as possible.
 * System audio: Patched sfml-audio to work better with oal-consumer and
    therefore removed PortAudio as the system audio implementation and went back
    to oal.
 * Flash: Changed so that the initial buffer fill of frames is rendered at a
    frame-duration pace instead of as fast as possible. Otherwise time based
    animations render incorrectly. During buffer recovery, a higher paced
    rendering takes place, but still not as fast as possible, which can cause
    animations to be somewhat incorrectly rendered. This is the only way though
    if we want the buffer to be able to recover after depletion.
 * Fixed race condition during server shutdown.
 * OSC: outgoing audio levels from the audio mixer for each audio channel is
    now transmitted (pFS and dBFS). (Thomas Kaltz III)
 * Stage: Fixed bug where tweened transforms were only ticked when a
    corresponding layer existed.
 * Screen consumer: Added borderless option and correct handling of name
    option. (Thomas Kaltz III)
 * AMCP: CLS now reports duration and framerate for MOVIE files were
    information is possible to extract. (Robert Nagy)
 * Version bump to keep up with CasparCG Client version.



CasparCG Server 2.0.4 Stable (as compared to 2.0.4 Beta 1)
==========================================================

General
-------
 * Can now open media with file names that only consist of digits.
    (Cambell Prince)
 * Miscellaneous stability and performance improvements.

Video mixer
-----------
 * Conditional compilation of chroma key support and straight alpha output
    support in shader (just like with blend-modes) because of performance impact
    even when not in use on a layer or on a channel. New <mixer /> element added
    to configuration for turning on mixer features that not everybody would want
    to pay for (performance-wise.) blend-modes also moved into this element.
 * Fixed bug where MIXER LEVELS interpreted arguments in the wrong order, so
    that gamma was interpreted as max_input and vice versa.

Consumers
---------
 * Added support for NewTek iVGA, which enables the use of CasparCG Server
    fill+key output(s) as input source(s) to a NewTek TriCaster without
    requiring video card(s) in the CasparCG Server machine, or taking up inputs
    in the TriCaster. <newtek-ivga /> element in config enables iVGA on a
    channel. (Robert Nagy sponsored by NewTek)
 * DeckLink: Created custom decklink allocator to reduce the memory footprint.
 * Replaced usage of SFML for <system-audio /> with PortAudio, because of
    problems with SFML since change to static linkage. Also PortAudio seems to
    give lower latency.

Producers
---------
 * FFmpeg: Added support for arbitrary FFmpeg options/parameters
    in ffmpeg_producer. (Cambell Prince)
 * Flash: Flash Player 11.8 now tested and fully supported.
 * Flash: No longer starts a Flash Player to service CG commands that mean
    nothing without an already running Flash Player.
 * Flash: globally serialize initialization and destruction of Flash Players,
    to avoid race conditions in Flash.
 * Flash: changed so that the Flash buffer is filled with Flash Player
    generated content at initialization instead of empty frames.

OSC
---
 * Performance improvements. (Robert Nagy sponsored by Boffins Technologies)
 * Never sends old values to OSC receivers. Collects the latest value of each
    path logged since last UDP send, and sends the new UDP packet (to each
    subscribing OSC receiver) with the values collected. (Robert Nagy sponsored
    by Boffins Technologies)
 * Batches as many OSC messages as possible in an OSC bundle to reduce the
    number of UDP packets sent. Breakup into separate packages if necessary to
    avoid fragmentation. (Robert Nagy sponsored by Boffins Technologies)
 * Removed usage of Microsoft Agents library (Server ran out of memory after a
    while) in favour of direct synchronous invocations.



CasparCG Server 2.0.4 Beta 1 (as compared to 2.0.3 Stable)
==========================================================

General
-------
 * Front-end GUI for simplified configuration and easy access to common tasks.
    (Thomas Kaltz III and Jeff Lafforgue)
 * Added support for video and images file thumbnail generation. By default the
    media directory is scanned every 5 seconds for new/modified/removed files
    and thumbnails are generated/regenerated/removed accordingly.
 * Support for new video modes: 1556p2398, 1556p2400, 1556p2500, 2160p2398,
    2160p2400, 2160p2500, 2160p2997 and 2160p3000.
 * Experimental ATI graphics card support by using static linking against SFML
    instead of dynamic. Should improve ATI GPU support, but needs testing.
 * Added support for playback and pass-through of up to 16 audio channels. See
    http://casparcg.com/forum/viewtopic.php?f=3&t=1453 for more information.
 * Optimizations in AMCP protocol implementations for large incoming messages,
    for example base64 encoded PNG images.
 * Logging output now includes milliseconds and has modified format:
    YYYY-MM-DD hh:mm:ss.zzz
 * Improved audio playback with 720p5994 and 720p6000 channels.
 * An attempt to improve output synchronization of consumers has been made. Use
    for example:

    <consumers>
      <synchronizing>
        <decklink>
          <device>1</device>
          <embedded-audio>true</embedded-audio>
        </decklink>
        <decklink>
          <device>2</device>
          <key-only>true</key-only>
        </decklink>
      </synchronizing>
    </consumers>

    ...to instruct the server to keep both DeckLink consumers in sync with each
    other. Consider this experimental, so don't wrap everything in
    <synchronizing /> unless synchronization of consumer outputs is needed. For
    synchronization to be effective all synchronized cards must have genlock
    reference signal connected.
 * Transfer of source code and issue tracker to github. (Thomas Kaltz III)

Layer
-----
 * Fixed a problem where the first frame was not always shown on LOAD.
    (Robert Nagy)

Stage
-----

 * Support for layer consumers for listening to frames coming out of producers.
    (Cambell Prince)

Audio mixer
-----------
 * Added support for a master volume mixer setting for each channel.

Video mixer
-----------
 * Added support for chroma keying. (Cambell Prince)
 * Fixed bug where MIXER CONTRAST set to < 1 can cause transparency issues.
 * Experimental support for straight alpha output.

Consumers
---------
 * Avoid that the FFmpeg consumer blocks the channel output when it can't keep
    up with the frame rate (drops frames instead).
 * Added support for to create a separate key and fill file when recording with
    the FFmpeg consumer. Add the SEPARATE_KEY parameter to the FFmpeg consumer
    parameter list. The key file will get the _A file name suffix to be picked
    up by the separated_producer when doing playback.
 * The Image consumer now writes to the media folder instead of the data
    folder.
 * Fixed bug in DeckLink consumer where we submit too few audio samples to the
    driver when the video format has a frame rate > 50.
 * Added another experimental DeckLink consumer implementation where scheduled
    playback is not used, but a similar approach as in the bluefish consumer
    where we wait for a frame to be displayed and then display the next frame.
    It is configured via a <blocking-decklink> consumer element. The benefits of
    this consumer is lower latency and more deterministic synchronization
    between multiple instances (should not need to be wrapped in a
    <synchronizing> element when separated key/fill is used).

Producers
---------
 * Added support for playing .swf files using the Flash producer. (Robert Nagy)
 * Image producer premultiplies PNG images with their alpha.
 * Image producer can load a PNG image encoded as base64 via:
    PLAY 1-0 [PNG_BASE64] <base64 string>
 * FFmpeg producer can now use a directshow input filters:
    PLAY 1-10 "dshow://video=Some Camera"
    (Cambell Prince, Julian Waller and Robert Nagy)
 * New layer producer which directs the output of a layer to another layer via
    a layer consumer. (Cambell Prince)

AMCP
----
 * The master volume feature is controlled via the MASTERVOLUME MIXER
    parameter. Example: MIXER 1 MASTERVOLUME 0.5
 * THUMBNAIL LIST/RETRIEVE/GENERATE/GENERATE_ALL command was added to support
    the thumbnail feature.
 * ADD 1 FILE output.mov SEPARATE_KEY activates the separate key feature of the
    FFmpeg consumer creating an additional output_a.mov containing only the key.
 * Added KILL command for shutting down the server without console access.
 * Added RESTART command for shutting down the server in the same way as KILL
    except that the return code from CasparCG Server is 5 instead of 0, which
    can be used by parent process to take other actions. The
    'casparcg_auto_restart.bat' script restarts the server if the return code is
    5.
 * DATA RETRIEVE now returns linefeeds encoded as an actual linefeed (the
    single character 0x0a) instead of the previous two characters:
    \ followed by n.
 * MIXER CHROMA command added to control the chroma keying. Example:
    MIXER 1-1 CHROMA GREEN|BLUE 0.10 0.04
    (Cambell Prince)
 * Fixed bug where MIXER FILL overrides any previous MIXER CLIP on the same
    layer. The bug-fix also has the side effect of supporting negative scale on
    MIXER FILL, causing the image to be flipped.
 * MIXER <ch> STRAIGHT_ALPHA_OUTPUT added to control whether to output straight
    alpha or not.
 * Added INFO <ch> DELAY and INFO <ch>-<layer> DELAY commands for showing some
    delay measurements.
 * PLAY 1-1 2-10 creates a layer producer on 1-1 redirecting the output of
    2-10. (Cambell Prince)

OSC
---
 * Support for sending OSC messages over UDP to either a predefined set of
    clients (servers in the OSC sense) or dynamically to the ip addresses of the
    currently connected AMCP clients.
    (Robert Nagy sponsored by Boffins Technologies)
 * /channel/[1-9]/stage/layer/[0-9]
   * always             /paused           [paused or not]
   * color producer     /color            [color string]
   * ffmpeg producer    /profiler/time    [render time]     [frame duration]
   * ffmpeg producer    /file/time        [elapsed seconds] [total seconds]
   * ffmpeg producer    /file/frame       [frame]           [total frames]
   * ffmpeg producer    /file/fps         [fps]
   * ffmpeg producer    /file/path        [file path]
   * ffmpeg producer    /loop             [looping or not]
   * during transitions /transition/frame [current frame]   [total frames]
   * during transitions /transition/type  [transition type]
   * flash producer     /host/path        [filename]
   * flash producer     /host/width       [width]
   * flash producer     /host/height      [height]
   * flash producer     /host/fps         [fps]
   * flash producer     /buffer           [buffered]        [buffer size]
   * image producer     /file/path        [file path]



CasparCG Server 2.0.3 Stable (as compared to 2.0.3 Alpha)
=========================================================

Stage
-----

 * Fixed dead-lock that can occur with multiple mixer tweens. (Robert Nagy)

AMCP
----

 * DATA STORE now supports creating folders of path specified if they does not
    exist. (Jeff Lafforgue)
 * DATA REMOVE command was added. (Jeff Lafforgue)



CasparCG Server 2.0.3 Alpha (as compared to 2.0 Stable)
=======================================================

General
-------

 * Data files are now stored in UTF-8 with BOM. Latin1 files are still
    supported for backwards compatibility.
 * Commands written in UTF-8 to log file but only ASCII characters to console.
 * Added supported video formats:
   * 720p2398 (not supported by DeckLink)
   * 720p2400 (not supported by DeckLink)
   * 1080p5994
   * 1080p6000
   * 720p30 (not supported by DeckLink)
   * 720p29.976 (not supported by DeckLink)

CLK
---

 * CLK protocol implementation can now serve more than one connection at a time
    safely.
 * Added timeline support to the CLK protocol.
 * Refactored parts of the CLK parser implementation.

Consumers
---------

 * Consumers on same channel now invoked asynchronously to allow for proper
    sync of multiple consumers.
 * System audio consumer:
   * no longer provides sync to the video channel.
 * Screen consumer:
   * Support for multiple screen consumers on the same channel
   * No longer spin-waits for vsync.
   * Now deinterlaces to two separate frames so for example 50i will no longer
      be converted to 25p but instead to 50p for smooth playback of interlaced
      content.
 * DeckLink consumer now logs whether a reference signal is detected or not.

Producers
---------

 * Image scroll producer:
   * Field-rate motion instead of frame-rate motion with interlaced video
      formats. This can be overridden by giving the PROGRESSIVE parameter.
   * SPEED parameter now defines pixels per frame/field instead of half pixels
      per frame. The scrolling direction is also reversed so SPEED 0.5 is the
      previous equivalent of SPEED -1. Movements are done with sub-pixel
      accuracy.
   * Fixed incorrect starting position of image.
   * Rounding error fixes to allow for more exact scrolling.
   * Added support for motion blur via a new BLUR parameter
   * Added PREMULTIPLY parameter to support images stored with straight alpha.



CasparCG Server 2.0 Stable (as compared to Beta 3)
==================================================

General
-------

 * Misc stability and performance fixes.

Consumers
---------

 * File Consumer
   * Changed semantics to more closely follow FFmpeg (see forums).
   * Added options, -r, -acodec, -s, -pix_fmt, -f and more.
 * Screen Consumer
   * Added vsync support.



CasparCG Server 2.0 Beta 3 (as compared to Beta 1)
==================================================

Formats
-------

 * ProRes Support
   * Both encoding and decoding.
 * NTSC Support
   * Updated audio-pipeline for native NTSC support. Previous implementation
      did not fully support NTSC audio and could cause incorrect behaviour or
      even crashes.

Consumers
---------

 * File Consumer added
   * See updated wiki or ask in forum for more information.
   * Should support anything FFmpeg supports. However, we will work mainly with
      DNxHD, PRORES and H264.
    - Key-only is not supported.
 * Bluefish Consumer
   * 24 bit audio support.
    - Embedded-audio does not work with Epoch cards.
 * DeckLink Consumer
   * Low latency enabled by default.
   * Added graphs for driver buffers.
 * Screen Consumer
   * Changed screen consumer square PAL to the more common wide-square PAL.
   * Can now be closed.
   * Fixed interpolation artifacts when running non-square video-modes.
   * Automatically deinterlace interlaced input.

Producers
---------

 * DeckLink Producer
   * Improved color quality be avoiding unnecessary conversion to BGRA.
 * FFMPEG Producer
   * Fixed missing alpha for (RGB)A formats when deinterlacing.
   * Updated buffering to work better with files with long audio/video
      interleaving.
   * Seekable while running and after reaching EOF. CALL 1-1 SEEK 200.
   * Enable/disable/query looping while running. CALL 1-1 LOOP 1.
   * Fixed bug with duration calculation.
   * Fixed bug with fps calculation.
   * Improved auto-transcode accuracy.
   * Improved seeking accuracy.
   * Fixed bug with looping and LENGTH.
   * Updated to newer FFmpeg version.
   * Fixed incorrect scaling of NTSC DV files.
   * Optimized color conversion when using YADIF filters.
 * Flash Producer
   * Release Flash Player when empty.
   * Use native resolution TemplateHost.
   * TemplateHosts are now chosen automatically if not configured. The
      TemplateHost with the corresponding video-mode name is now chosen.
   * Use square pixel dimensions.

AMCP
----

 * When possible, commands will no longer wait for rendering pipeline. This
    reduces command execution latencies, especially when sending a lot of
    commands in a short timespan.
 * Fixed CINF command.
 * ADD/REMOVE no longer require subindex,
    e.g. "ADD 1 SCREEN" / "REMOVE 1 SCREEN" instead of "ADD 1-1 SCREEN" / ...
 * PARAM is renamed to CALL.
 * STATUS command is replaced by INFO.
 * INFO command has been extended:
   * INFO (lists channels).
   * INFO 1 (channel info).
   * INFO 1-1 (layer info).
   * INFO 1-1 F (foreground producer info).
   * INFO 1-1 B (background producer info).
   * INFO TEMPLATE mytemplate (template meta-data info, e.g. field names).
 * CG INFO command has been extended.
   * CG INFO 1 (template-host information, e.g. what layers are occupied).

Mixer
-----

 * Fixed alpha with blend modes.
 * Automatically deinterlace for MIXER FILL commands.

Channel
-------

 * SET MODE now reverts back to old video-mode on failure.

Diagnostics
-----------

 * Improved graphs and added more status information.
 * Print configuration into log at startup.
 * Use the same log file for the entire day, instead of one per startup as
    previously.
 * Diagnostics window is now closable.



CasparCG Server 2.0 Beta 1 (as compared to Alpha)
=================================================

 * Blending Modes (needs to be explicitly enabled)
   * overlay
   * screen
   * multiply
   * and many more.
 * Added additive keyer in addition to linear keyer.
 * Image adjustments
   * saturation
   * brightness
   * contrast
   * min input-level
   * max input-level
   * min output-level
   * max output-level
   * gamma
 * Support for FFmpeg-filters such as (ee http://ffmpeg.org/libavfilter.html)
   * yadif deinterlacer (optimized in CasparCG for full multi-core support)
   * de-noising
   * dithering
   * box blur
   * and many more
 * 32-bit SSE optimized audio pipeline.
 * DeckLink-Consumer uses external-key by default.
 * DeckLink-Consumer has 24 bit embedded-audio support.
 * DeckLink-Producer has 24 bit embedded-audio support.
 * LOADBG with AUTO feature which automatically plays queued clip when
    foreground clip has ended.
 * STATUS command for layers.
 * LOG LEVEL command for log filtering.
 * MIX transition works with transparent clips.
 * Freeze on last frame.
 * Producer buffering is now configurable.
 * Consumer buffering is now configurable.
 * Now possible to configure template-hosts for different video-modes.
 * Added auto transcoder for FFmpeg producer which automatically transcodes
    input video into compatible video format for the channel.
   * interlacing (50p -> 50i)
   * deinterlacing (50i -> 25p)
   * bob-deinterlacing (50i -> 50p)
   * bob-deinterlacing and reinterlacing (w1xh150i -> w2xh250i)
   * doubling (25p -> 50p)
   * halfing (50p -> 25p)
   * field-order swap (upper <-> lower)
 * Screen consumer now automatically deinterlaces when receiving interlaced
    content.
 * Optimized renderer.
 * Renderer can now be run asynchronously with producer by using a
    producer-buffer size greater than 0.
 * Improved error and crash recovery.
 * Improved logging.
 * Added Image-Scroll-Producer.
 * Key-only has now near zero performance overhead.
 * Reduced memory requirements.
 * Removed "warm up lag" which occurred when playing the first media clip after
    the server has started.
 * Added read-back fence for OpenGL device for improved multi-channel
    performance.
 * Memory support increased from standard 2 GB to 4 GB on 64 bit Win 7 OS.
 * Added support for 2* DeckLink cards in Full HD.
 * Misc bugs fixes and performance improvements.
 * Color producer now support some color codes in addition to color codes, e.g.
    EMPTY, BLACK, RED etc...
 * Alpha value in color codes is now optional.
 * More than 2 DeckLink cards might be possible but have not yet been tested.



CasparCG Server 2.0 Alpha (as compared to 1.8)
==============================================

General
-------

 * Mayor refactoring for improved readability and maintainability.
 * Some work towards platform-independence. Currently the greatest challenge
    for full platform-independence is flash-producer.
 * Misc improved scalability.
 * XML-configuration.
 * DeckLink
   * Support for multiple DeckLink cards.

Core
----

 * Multiple producers per video_channel.
 * Multiple consumers per video_channel.
 * Swap producers between layers and channels during run-time.
 * Support for upper-field and lower-field interlacing.
 * Add and remove consumers during run-time.
 * Preliminary support for NTSC.

AMCP
----

 * Query flash and template-host version.
 * Recursive media-folder listing.
 * Misc changes.

Mixer
-----

 * Animated tween transforms.
 * Image-Mixer
   * Fully GPU accelerated (all features listed below are done on the GPU),
   * Layer composition.
   * Color spaces (rgba, bgra, argb, yuv, yuva, yuv-hd, yuva-hd).
   * Interlacing.
   * Per-layer image transforms:
     * Opacity
     * Gain
     * Scaling
     * Clipping
     * Translation
 * Audio Mixer
   * Per-layer and per-sample audio transforms:
       * Gain
   * Fully internal audio mixing. Single output video_channel.

Consumers
---------

 * DeckLink Consumer
   * Embedded audio.
   * HD support.
   * Hardware clock.
 * Bluefish Consumer
   * Drivers are loaded on-demand (server now runs on computers without
      installed Bluefish drivers).
   * Embedded audio.
   * Allocated frames are no longer leaked.

Producers
---------

 * Decklink Producer
   * Embedded audio.
   * HD support.
 * Color Producer
   * GPU accelerated.
 * FFMPEG Producer
   * Asynchronous file IO.
   * Parallel decoding of audio and video.
   * Color space transform are moved to GPU.
 * Transition Producer
   * Fully interlaced transition (previously only progressive, even when
      running in interlaced mode).
   * Per-sample mixing between source and destination clips.
   * Tween transitions.
 * Flash Producer
   * DirectDraw access (slightly improved performance).
   * Improved time-sync. Smoother animations and proper interlacing.
 * Image Producer
   * Support for various image formats through FreeImage library.

Diagnostics
-----------

 * Graphs for monitoring performance and events.
 * Misc logging improvements.
 * Separate log file for every run of the server.
 * Error logging provides full exception details, instead of only printing that
    an error has occurred.
 * Console with real-time logging output.
 * Console with AMCP input.

Removed
-------

 * Registry configuration (replaced by XML Configuration).
 * TGA Producer (replaced by Image Producer).
 * TGA Scroll Producer
