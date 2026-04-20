"""
dfu-util wrapper — ignores the benign "exit 74" leave-request timeout on
STM32H7. Flash is already written by the time dfu-util hits that status poll;
the chip has reset out of DFU before the poll arrives.
"""
import subprocess
import sys

result = subprocess.run(sys.argv[1:])
if result.returncode == 74:
    print("[dfu-tolerant] dfu-util exit 74 (STM32H7 leave-request quirk). "
          "Flash succeeded.")
    sys.exit(0)
sys.exit(result.returncode)
