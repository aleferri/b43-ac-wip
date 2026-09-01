#!/usr/bin/env python3
"""Episode where a sweep segment's up phase ends.

Segments are cut on the "up" event, so each one holds a complete up cycle plus
the head of the next down: once the up finishes the capture continues with
mhf_config, MAC.MCTRL and the MAC shared-memory block that open the following
cycle. Comparing the up phase against the whole segment therefore runs off the
end of the phase.

The MHF sequence is the marker for that boundary, and its first op is
MAC.MHF on 0x0004 with mask 0x0080.
"""

import re
import sys

MARK = re.compile(r"\s*[\d.]+\s+#(\d+)\s+cpu\d+\s+MAC\.MHF\s+addr=0x0*4\s+"
                  r"val=0x0*80\s+mask=0x0*80")

for line in open(sys.argv[1], errors="replace"):
    m = MARK.match(line)
    if m:
        print(m.group(1))
        break
