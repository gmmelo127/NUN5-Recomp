import re
import sys
from pathlib import Path
p = Path(sys.argv[1] if len(sys.argv) > 1 else 'generated/naruto/vu1_table.cpp')
try:
    m = re.search(r'register_programs\([^,]+,\s*(\d+)u\)', p.read_text())
    print(m.group(1) if m else '0')
except OSError:
    print('0')
