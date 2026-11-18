// fastdl - tell game clients where to download this server's maps and mods.
//
// Adds the FastDL (HTTP download) server to the server's GameSpy query replies
// as \fastdlport\<port>, \fastdlhost\<host> and \fastdlpath\<path>. A patched
// client (the Battlefield 1942 patch's FastDL) queries the server it joins,
// downloads what it is missing from http://<host>:<port><path>/..., then joins.
// The download server itself is any web server serving the game folder's
// layout; this plugin only advertises it.
//
//   fastdl.port   0 = off
//   fastdl.host   empty = the game server's own address
//   fastdl.path   URL path of the files, e.g. /bf1942
//
// Needs query rules (RMM_CAP_QUERY_RULES).
#include <rmm/rmm.hpp>

#include <string>

namespace {

const char* const kKeys[] = {"fastdlport", "fastdlhost", "fastdlpath"};

}  // namespace

class FastDl : public rmm::Plugin {
public:
    bool OnLoad(bool, std::string& error) override {
        if (!rmm::HasCap(RMM_CAP_QUERY_RULES)) {
            error = "fastdl needs query rules, which this server doesn't offer (rmm caps)";

            return false;
        }

        port_ = rmm::Var("fastdl.port", "0", "Port of the FastDL (HTTP) server the clients download from (0 = off)",
                         RMM_VAR_NUMERIC);
        port_.Bounds(0, 65535);
        host_ = rmm::Var("fastdl.host", "", "Host name or address of the FastDL server (empty = this server's)");
        path_ = rmm::Var("fastdl.path", "", "URL path of the game files on the FastDL server, e.g. /bf1942");

        for (rmm::Var* v : {&port_, &host_, &path_})
            v->OnChange([this](const std::string&, const std::string&) { Apply(); });

        rmm::Command("fastdl", RMM_CMD_ALL, 0, "Where clients download this server's files from", "fastdl",
                     [this](rmm::CmdArgs& a) {
                         a.Reply(advertised_.empty() ? "FastDL is not advertised (fastdl.port 0)."
                                                     : "Clients download from " + advertised_);
                     });
        Apply();

        return true;
    }

private:
    void Apply() {
        for (const char* k : kKeys)
            rmm::RemoveQueryRule(k);

        advertised_.clear();
        int port = port_.Int();
        std::string host = host_.Str(), path = path_.Str();

        if (port <= 0)
            return;

        if (!path.empty() && path[0] != '/')
            path = "/" + path;

        // The replies are \key\value: a backslash would end the value.
        if (host.find('\\') != std::string::npos || path.find('\\') != std::string::npos) {
            rmm::Warn("fastdl.host and fastdl.path can't contain '\\'; FastDL is not advertised");

            return;
        }

        bool ok = rmm::SetQueryRule("fastdlport", std::to_string(port));

        if (ok && !host.empty())
            ok = rmm::SetQueryRule("fastdlhost", host);

        if (ok && !path.empty())
            ok = rmm::SetQueryRule("fastdlpath", path);

        if (!ok) {
            for (const char* k : kKeys)
                rmm::RemoveQueryRule(k);

            rmm::Warn("FastDL is not advertised (see the warning above)");

            return;
        }

        std::string before = logged_;
        advertised_ = logged_ =
            rmm::Format("http://%s:%d%s", host.empty() ? "<this server>" : host.c_str(), port, path.c_str());

        if (advertised_ != before)
            rmm::Info("Query replies say the FastDL server is %s", advertised_.c_str());
    }

    rmm::Var port_, host_, path_;
    std::string advertised_, logged_;
};

RMM_DECLARE_PLUGIN(FastDl, "FastDL", "1.0.0", "RMM Team", "Advertises the FastDL server in the server's query replies",
                   "", "fastdl", RMM_GAME_ANY, 0)
