// Contract handling: bounded file loading and validation of the fixed
// version-1 application schema.
#pragma once

#include "model.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace execcontract {

struct ContractText {
    std::optional<std::string> text;
    std::optional<ContractError> error;
};

// Open `path` (relative paths resolve against the current working directory),
// require a regular file of at most `cap` bytes and read it. Opened with
// O_NONBLOCK so a FIFO cannot block the probe before it is rejected.
ContractText load_contract_file(const std::string& path, std::size_t cap = kMaxContractBytes);

struct ContractResult {
    std::optional<Contract> contract;
    std::optional<ContractError> error;
};

// Parse and validate. Never partially accepts: any problem yields an error.
ContractResult parse_contract(std::string_view text);

// Names of the areas the contract does not check, in canonical order.
std::vector<std::string> unchecked_areas(const Contract& contract);

}  // namespace execcontract
