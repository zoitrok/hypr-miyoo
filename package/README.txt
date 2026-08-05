HYPR demoscene radio - @VERSION@
================================

An 80s-vaporwave internet-radio player for the Miyoo Mini Plus (OnionOS).
It streams hypr.website - a demoscene music radio - and shows what's playing
over an endless neon grid, a scanline sun, and an audio-reactive spectrum.

  https://hypr.website
  https://github.com/zoitrok/hypr-miyoo


What you need
-------------
  - A Miyoo Mini PLUS (the Plus has WiFi; the original Mini does not).
  - OnionOS installed.
  - WiFi configured and connected.


Install
-------
  1. Unzip this archive.
  2. Copy the "App" folder onto the root of your SD card, merging it with the
     existing /mnt/SDCARD/App - this drops "Hypr" in beside your other apps.
     (Nothing is overwritten.)
  3. Put the card back, boot the device, and open HYPR from the Apps menu.

  Upgrading from an earlier version: do the same thing and let it overwrite
  App/Hypr. Your hypr.conf is overwritten too, so save a copy first if you
  edited it.

  First launch takes a few seconds: it sets the clock from the network (the
  device has no battery clock), then connects and starts playing.


Controls
--------
  A .......... next page (Now Playing / Up Next / Oneliner)
  UP/DOWN .... scroll the lists
  SELECT ..... toggle the debug overlay (buffer, bitrate, reconnects)
  B or MENU .. quit
  VOLUME ..... handled by OnionOS as usual


Notes
-----
  - This is a listen-only player. There is no login, voting, or chatting yet -
    those need an on-screen keyboard, which is deferred.
  - It needs an internet connection. If WiFi drops it shows RECONNECTING and
    recovers on its own.
  - Settings live in App/Hypr/hypr.conf (stream URL, etc.). The defaults point
    at hypr.website and need no editing.
  - A log is written to App/Hypr/hypr-log.txt (overwritten each launch) if you
    ever need to see what happened.


What changed in 0.1.1
---------------------
  Fixes a failure where the app stopped connecting and logged TLS certificate
  errors after the server renewed its certificate.

  The cause was the clock, not the certificate. The Miyoo has no battery-backed
  clock, so it starts from whatever time was saved when it was last switched
  off. A device that sat unused for a few weeks therefore booted with a date
  weeks in the past - late enough to look like a real date, so the app accepted
  it and never corrected it. A certificate issued after that date reads as "not
  yet valid", every connection attempt fails on the date check, and retrying
  cannot help.

  The app now checks its clock against the network on every launch instead of
  only when the clock is obviously unset, and it only ever moves the clock
  forward, never back.

  If your device is stuck on the previous version, installing this one fixes it
  on the next launch. Nothing else about the app changes.


Version 0.1.1. Licensed under Apache-2.0 (see LICENSE).
Not affiliated with the HYPR backend; this is an independent client.
