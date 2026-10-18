#ifndef slic3r_U1SendDialog_hpp_
#define slic3r_U1SendDialog_hpp_

#include "GUI_Utils.hpp"
#include "libslic3r/U1PrintTask.hpp"

#include <boost/filesystem/path.hpp>

#include <vector>

class wxCheckBox;
class wxChoice;
class wxTextCtrl;

namespace Slic3r {
namespace GUI {

/// Native replacement for the Flutter pre-print dialog on the Snapmaker U1:
/// file name, filament -> toolhead mapping, and the printer's print options.
class U1SendDialog : public DPIDialog
{
public:
    /// *plate_filaments* is indexed by slicer filament; entries the plate does
    /// not use have present == false and get no row.
    U1SendDialog(wxWindow                               *parent,
                 const boost::filesystem::path          &default_name,
                 const std::vector<U1PrintTask::Filament> &plate_filaments,
                 const U1PrintTask::PrinterState          &printer);

    boost::filesystem::path filename() const;
    bool                    start_print() const { return m_start_print; }
    U1PrintTask::Options    options() const;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void on_confirm(bool start_print);

    struct Row
    {
        size_t    filament;
        wxChoice *toolhead;
    };

    std::vector<U1PrintTask::Filament> m_plate_filaments;
    std::vector<U1PrintTask::Filament> m_toolheads;
    std::vector<Row>                   m_rows;
    wxTextCtrl                        *m_filename          = nullptr;
    wxCheckBox                        *m_bed_level         = nullptr;
    wxCheckBox                        *m_flow_calibrate    = nullptr;
    wxCheckBox                        *m_time_lapse        = nullptr;
    wxCheckBox                        *m_shaper_calibrate  = nullptr;
    bool                               m_start_print       = true;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_U1SendDialog_hpp_
