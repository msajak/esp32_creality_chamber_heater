#include "shared_objs.h"

std::string to_string(FaultReason fault_reason) {
    if (fault_reason == FaultReason::NONE) return "None";

    std::string result;
    if (check_fault_reason(fault_reason, FaultReason::HEAT_TIMEOUT))    result += "HEAT_TIMEOUT | ";
    if (check_fault_reason(fault_reason, FaultReason::TEMP_READ))       result += "TEMP_READ | ";
    if (check_fault_reason(fault_reason, FaultReason::TEMP_RANGE))      result += "TEMP_RANGE | ";
    if (check_fault_reason(fault_reason, FaultReason::TEMP_FILT))       result += "TEMP_FILT | ";
    if (check_fault_reason(fault_reason, FaultReason::TEMP_FILT_RANGE)) result += "TEMP_FILT_RANGE | ";
    if (check_fault_reason(fault_reason, FaultReason::TEMP_STUCK))      result += "TEMP_STUCK | ";
    if (check_fault_reason(fault_reason, FaultReason::FAN_TIMEOUT))     result += "FAN_TIMEOUT | ";
    if (check_fault_reason(fault_reason, FaultReason::FAN_DROP))        result += "FAN_DROP | ";

    if (!result.empty()) {
        result.erase(result.length() - 3);
    }
    return result;
}
