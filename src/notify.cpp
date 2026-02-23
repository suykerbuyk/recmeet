// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "notify.h"

#include <libnotify/notify.h>
#include <cstdio>

namespace recmeet {

void notify_init() {
    ::notify_init("recmeet");
}

void notify(const std::string& title, const std::string& body) {
    try {
        NotifyNotification* n = notify_notification_new(
            title.c_str(), body.empty() ? nullptr : body.c_str(), nullptr);
        if (n) {
            notify_notification_show(n, nullptr);
            g_object_unref(G_OBJECT(n));
        }
    } catch (...) {
        // Fire-and-forget
    }
}

void notify_cleanup() {
    ::notify_uninit();
}

} // namespace recmeet
