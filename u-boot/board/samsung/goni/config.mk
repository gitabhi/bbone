#
# Copyright (C) 2010 Samsung Electronics
# Kyungmin Park <kyungmin.park@samsung.com>
#
# SPDX-License-Identifier:	GPL-2.0+
#

# On S5PC100 we use the 128 MiB OneDRAM bank at
#
# 0x30000000 to 0x35000000 (80MiB)
# 0x38000000 to 0x40000000 (128MiB)
#
# On S5PC110 we use the 128 MiB OneDRAM bank at
#
# 0x30000000 to 0x35000000 (80MiB)
# 0x40000000 to 0x50000000 (256MiB)
#
CONFIG_SYS_TEXT_BASE = 0x34800000