#include <numeric_input_config_common.hpp>

#include <Configuration.h>

const NumericInputConfig numeric_input_config::nozzle_temperature = {
    .max_value = HEATER_0_MAXTEMP - HEATER_MAXTEMP_SAFETY_MARGIN,
    .special_value = 0,
    .unit = Unit::celsius,
};

const NumericInputConfig numeric_input_config::filament_nozzle_temperature = {
    .min_value = EXTRUDE_MINTEMP,
    .max_value = HEATER_0_MAXTEMP - HEATER_MAXTEMP_SAFETY_MARGIN,
    .unit = Unit::celsius,
};

const NumericInputConfig numeric_input_config::bed_temperature = {
    .max_value = (BED_MAXTEMP - BED_MAXTEMP_SAFETY_MARGIN),
    .special_value = 0,
    .unit = Unit::celsius,
};
