#! usr/bin/bash

cd d:/Projects/pymodbus/repl/
source .venv/Scripts/activate
pipx run poetry run pymodbus.console serial --port COM11 --baudrate 115200 --parity N --timeout 2