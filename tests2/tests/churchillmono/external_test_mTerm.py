#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.

# This software may be used and distributed according to the terms of the
# GNU General Public License version 2.

import unittest

from common.base_mTerm_test import BaseMTermTest
from utils.test_utils import qemu_check


@unittest.skipIf(qemu_check(), "test env is QEMU, skipped")
class MTermTest(BaseMTermTest, unittest.TestCase):
    pass
