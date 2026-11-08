---
name: Bug report
about: Create a report to help us improve
title: ''
labels: bug
assignees: ''

---

**Describe the bug**
A clear and concise description of what the bug is.

**To Reproduce**
Steps to reproduce the behavior:
1. Go to '...'
2. Click on '....'
3. Scroll down to '....'
4. See error

**Expected behavior**
A clear and concise description of what you expected to happen.

**Environment**
 - Modem model and firmware: [`quectel show device <device>` reports both]
 - OpenWrt version: [e.g. 25.12.6]
 - Asterisk version: [e.g. 23.1.0]
 - chan_quectel version: [`quectel show version`]

**Logs**
Relevant output from `/var/log/asterisk/messages.log`. For audio problems please
also include `quectel show device <device>`, which reports the frame counters
and xruns.

**Additional context**
Add any other context about the problem here.
