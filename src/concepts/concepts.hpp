// PS5 Share hub - the only UI design in the app.
// Copyright (C) 2026 PS5 Share contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/concept.hpp"

#include <memory>

namespace hui::concepts
{

// The Share Hub: capture gallery, detail panel, settings. Additional app
// screens arrive as further designs listed here and in registry.cpp.
std::unique_ptr<app::Concept> make_ps5share(app::Context &context);

} // namespace hui::concepts
