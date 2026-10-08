#!/usr/bin/env python3
"""Qualify an actual published208-byte Native AOT LayerGame artifact.

This runner never publishes or compiles; use the real platform artifact and a
preserved pre-layer executable. Shared checks are in runtime_gameplay_layers.py.
"""
# SPDX-License-Identifier: Apache-2.0
from runtime_gameplay_layers import main
if __name__=='__main__':main(native_aot=True)
