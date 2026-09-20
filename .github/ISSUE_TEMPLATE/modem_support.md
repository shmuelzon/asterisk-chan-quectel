---
name: Modem support
about: Report results with a Quectel module other than the EC25-E
title: ''
labels: enhancement
assignees: ''

---

**Module**
Model and firmware revision, as reported by `quectel show device <device>`.

**What works / what does not**
Voice in each direction, SMS send, SMS receive, caller id, DTMF.

**Audio interface**
Does `AT+QPCMV=1,2` succeed, and does an ALSA capture device appear
(`cat /proc/asound/cards`)? If the module uses PCM over serial instead, say so -
that path is not currently implemented.

**Additional context**
Anything else that might help, e.g. AT commands the module needs that the EC25-E
does not.
