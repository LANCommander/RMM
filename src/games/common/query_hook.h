// The server's own UDP sends, so its GameSpy query replies carry the plugins'
// query rules (Core::Query, rmm_game_api::set_query_rule).
//
//  - Windows: the main executable's WSOCK32/WS2_32 sendto import.
//  - Linux: librmm.so exports sendto, which interposes on libc's because the
//    module is LD_PRELOADed; Install() only switches the rewriting on.
#pragma once

#include <string>

namespace rmm::queryhook {

/// @return false (with @p err) if the server's sends can't be hooked.
bool Install(std::string& err);
/// Stop rewriting (the import stays patched, as a pass-through).
void Remove();

}  // namespace rmm::queryhook
