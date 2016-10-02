#!/bin/bash

sudo sh -c "echo 1 >/proc/idlemon"
"$@"
cat /proc/idlemon

