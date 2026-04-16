# alsa-dsd-native

Play DSD audio with DSD Native (not DOP). Only *.dff files supported.

## Usage

```
alsa-dsd-native [OPTION]... [FILE]... 

Options:
 -D,--audio-device       Manually set an alsa device
```

The program automatically selects sound card that supports DSD Native.

If that fails, check your sound card by the following way.

- `cat /proc/asound/cards` to find out the card id, e.g. `3`

- `cat /proc/asound/card3/stream0` and check if the card supports `Format: DSD_U32_BE`

