#!/bin/bash

for n in {1..6}; do cd ~/src/atari8EspBusBang.$n; pwd; $@; done

