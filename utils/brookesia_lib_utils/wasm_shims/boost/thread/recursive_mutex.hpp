/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

// Emscripten's single-threaded Brookesia build provides the Boost mutex API
// through the standard-library aliases declared by this compatibility header.
#include "boost/thread/mutex.hpp"
