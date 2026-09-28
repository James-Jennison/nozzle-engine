// Bambu-style extruder variants (from upstream OrcaSlicer / BambuStudio). A multi-extruder or multi-nozzle-type printer
// profile stores many per-extruder settings once per extruder variant ("Direct Drive Standard", "Direct Drive High
// Flow", ...), listed in extruder_variant_list per extruder and keyed by *_extruder_id / *_extruder_variant. Before
// slicing, those vectors are collapsed to one value per extruder (printer and process settings) or per filament
// (filament settings), for the variant each extruder has installed. AGPL-3.0, like the code it is ported from.
#pragma once

#include "PrintConfig.hpp"

namespace Slic3r {

// Upstream DynamicPrintConfig::support_different_extruders: the printer has more than one extruder variant.
bool support_different_extruders(const DynamicPrintConfig &config, int &extruder_count);

// Collapses the variant vectors in place, as upstream Print::apply does (static filament_map; no per-layer nozzle
// grouping yet). Returns false, changing nothing, for a printer without extruder variants.
bool collapse_extruder_variants(DynamicPrintConfig &config);

// True once collapse_extruder_variants() has run on this config: per-extruder vectors then hold one value per extruder.
bool extruder_variants_collapsed(const ConfigBase &config);

} // namespace Slic3r
