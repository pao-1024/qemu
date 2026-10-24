ESP32-S3 voice board peripherals
================================

The ``esp32s3`` machine provides an ``esp32s3.peripherals`` child at
``/machine/soc/peripherals`` for the PAO voice board. It runs ESP-IDF's I2S,
I2C, RMT and GPIO drivers. Networking uses the existing OpenETH device.

The supported board has a 16 kHz microphone on I2S1, a 24 kHz speaker on
I2S0, one WS2812 on GPIO48, a GPIO0 button, and a 128x64 SSD1306 at I2C0
address 0x3c. I2S samples use the board's mono 32-bit slots: microphone
PCM is shifted left 12 bits and speaker PCM is shifted right 16 bits.
This is a functional model of this board, not a general I2S format or
electrical timing model. GPIO matrix routing, analog audio characteristics,
Wi-Fi radio, arbitrary I2C devices, and RMT receive are not modeled.

Audio files
-----------

Supply raw signed 16-bit little-endian mono PCM files::

  -global driver=esp32s3.peripherals,property=pcm-in,value=input.pcm
  -global driver=esp32s3.peripherals,property=pcm-out,value=speaker.pcm

Input defaults to silence. Set the writable QOM ``input-active`` property
to 1 to inject the input file; EOF produces silence. The read-only
``rx-bytes`` and ``tx-bytes`` properties count bytes transferred through
guest GDMA descriptors. The virtual clock schedules DMA completion events.

Controls and display
--------------------

Use QMP ``qom-set`` on ``gpio-inputs`` to set external GPIO levels. GPIO0
is active low; the default input mask is all ones. GPIO interrupt status
and per-core status registers reflect configured edge/level events.

Read ``led-rgb`` using ``qom-get`` for the WS2812 RGB value. Read
``oled-pbm`` for a text PBM image of the SSD1306 display RAM. OLED contrast,
scan direction and analog brightness are not modeled.

The read-only ``gpio-outputs`` property reports direct GPIO output latches,
gated by output-enable, as a 49-bit mask. Peripheral-routed waveforms are
not decoded by this property. Host-side functional models can observe digital outputs
without adding a guest register or changing GPIO state. The PAO probe uses
it to check both H-bridge channels across all digital input combinations;
it does not model motor mechanics or electrical characteristics.

Reset clears peripheral state and rewinds the input file. Speaker output
continues in the same file. Live migration and VM snapshots are unsupported.

Verification
------------

The PAO repository's ``firmware/simulation/probe`` runs real ESP-IDF
drivers and verifies I2S sample content, NVS persistence and GPIO button
levels. Its QMP runner checks the full OLED bitmap and WS2812 RGB value
and repeats the probe across a machine reset. A separate voice runner
verifies HTTPS provisioning, WebSocket audio, ASR, text responses and
speaker output against the configured service.

See https://github.com/pao-1024/pao for the build and runtime commands.

WakeNet floating point
----------------------

The Xtensa FPU implements ``recip0.s``, ``rsqrt0.s`` and ``maddn.s`` used
by the ESP-SR WakeNet model. MADDN uses round-to-nearest-even independently
of FCR and leaves exception flags unchanged. The reciprocal seed operations
use SoftFloat results rather than a bit-exact hardware approximation table;
this supports functional inference, not hardware seed or timing conformance.
Instruction semantics follow the Cadence Xtensa ISA summary:
https://www.cadence.com/content/dam/cadence-www/global/en_US/documents/tools/silicon-solutions/compute-ip/isa-summary.pdf

The PAO probe exercises the seeds, negative zero, negative square root and
MADDN rounding/flags across reset. The wake runner injects spoken audio
through I2S and verifies WakeNet detection followed by a cloud conversation;
a negative run checks ordinary speech and silence without a wakeup.
