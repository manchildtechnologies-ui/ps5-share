// PS5 Share hub - the list of UI designs, in switcher order.
// Copyright (C) 2026 PS5 Share contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/concepts.hpp"

namespace hui::app
{

std::span<const ConceptFactory> concept_registry()
{
    static constexpr ConceptFactory kFactories[] = {
        concepts::make_ps5share,
    };
    return kFactories;
}

} // namespace hui::app
