#ifndef slic3r_MSlicerSDCPHost_hpp_
#define slic3r_MSlicerSDCPHost_hpp_

#include "PrintHost.hpp"

namespace Slic3r {

class MSlicerSDCPHost final : public PrintHost
{
public:
    explicit MSlicerSDCPHost(DynamicPrintConfig *config);

    const char *get_name() const override { return "ChiTu SDCP (local LAN)"; }
    bool test(wxString &message) const override;
    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString &message) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool has_auto_discovery() const override { return false; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return {}; }
    std::string get_host() const override { return m_host; }
    std::string get_unusable_symbols() const { return "\\/:*?\"%<>|[]"; }

private:
    std::string m_host;
    uint32_t m_resolution_x { 0 };
    uint32_t m_resolution_y { 0 };
};

} // namespace Slic3r

#endif
