Mumble fork - video-enabled client (Linux x86_64)
================================================

Run it:

    ./mumble

Everything it needs is in this folder. There is no installer; unzip it anywhere and run it.

BUILD-INFO.txt records the commit this was built from and the checksum of the binary inside, so you can
tell two downloads apart without unpacking and diffing them.

FIRST RUN
---------
If a camera is present, the Video Wizard runs once on first launch. It picks a camera, shows a live
preview so you can confirm it works, and then encodes real frames at several bitrates to measure what
each actually costs -- so the recommendation is based on your camera rather than on a guess.

You can run it again any time from:

    Configure  ->  Video Wizard

HOW TO SHARE YOUR CAMERA
------------------------
There is a camera button in the toolbar, next to mute, deafen and record. It is grey when idle and
green with a red dot while you are sharing. The same thing lives in the menu under Self -> Share Camera.

Nothing appears until you are connected to a server.

Video appears in a "Video" panel across the top half of the window, which stays hidden until somebody
is sharing. Your own camera is shown there too, labelled "You"; everyone else's tile carries their name.

All the individual settings live under:

    Configure  ->  Settings  ->  Video

That page has a live preview which encodes AND decodes, so it shows what viewers will really see, with
the measured bitrate underneath it.

If it says "No camera is available", the client could not find a capture device. Check that /dev/video0
exists and that your user can read it (on most distributions that means being in the "video" group).

Two clients on one machine cannot share the same camera: V4L2 gives exclusive access to whichever
opened it first, and the second is told the camera is unavailable. To run two locally you need two
cameras, or a virtual one.

REQUIREMENTS
------------
Linux x86_64, glibc 2.41 or newer (built on Fedora 42). The bundle includes Qt 6, its multimedia
backend, libvpx, protobuf and OpenSSL, so nothing needs to be installed first. Windows builds (wizard
installer, MSI, portable zip) and a flatpak are published alongside this one, all from the same
commit. There is no macOS build.

WHAT IS DIFFERENT FROM UPSTREAM MUMBLE
--------------------------------------
  * Camera sharing, as above.
  * Video uses its own UDP channel with its own crypto and sequence space, so video loss cannot
    degrade voice quality.
  * Two codecs: VP8 for cameras (~0.5 Mbit/s at 480p) and tiled JPEG for screen content, where
    unchanged regions cost nothing to send.
  * A new subscriber asks the sender for an immediate keyframe instead of waiting for the next
    scheduled one, and a receiver whose picture is stuck after packet loss does the same. The server
    rate-limits these to one per sender per second.
  * Two new ACL privileges, ShareVideo and ReceiveVideo, both granted by default.

Screen sharing: the button next to the camera one asks your desktop (via the XDG portal) what to
share - the dialog you see is your compositor's own, and its answer decides what is captured. Needs a
desktop portal, which every mainstream Wayland desktop ships; the button hides itself if none answers.

This client CANNOT talk to a stock Mumble server, and stock clients cannot use video with this one.
It is a clean-break fork: the UDP media framing carries an extra channel byte.

The server will present a self-signed certificate the first time you connect. That is expected; accept
it once and it is remembered.

FIXED SINCE THE FIRST RELEASE
-----------------------------
  * Received frames were only ever decoded as JPEG, so VP8 - the default for cameras - was discarded
    silently and you saw only your own picture.
  * A stream was announced only when it started, so whoever joined second was never told about it and
    subscribed to nothing. This one needs the matching server; an older server still behaves this way.
  * Opening a camera another program already held crashed the client instead of reporting it.
  * The video panel could divide by a tile count of zero and take the client down with it.
  * New subscribers waited up to four seconds for the next scheduled keyframe; they now get one
    immediately (needs the matching server).

CAVEAT WORTH READING
--------------------
The video transport contains new cryptographic code written for this fork. The construction is
conventional (explicit sequence number, sequence-derived nonce, sliding replay window) and is covered
by tests, but it has had no independent security review. Treat it accordingly.
