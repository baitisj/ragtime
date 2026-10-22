#include <cstdio>
#include <cstring>

#include <wx/cmdline.h>
#include <wx/fileconf.h>
#include <wx/socket.h>
#include <wx/wx.h>

#include "MainFrame.h"
#include "ragtime/Version.h"

class RagtimeApp : public wxApp
{
public:
    bool OnInit() override
    {
        SetAppName("Ragtime");
        if (!wxApp::OnInit())
            return false;

        // wxSocket wants initialising on the main thread before any socket.
        wxSocketBase::Initialize();
        auto* frame = new MainFrame();
        frame->Show();
        return true;
    }

    int OnExit() override
    {
        wxSocketBase::Shutdown();
        return wxApp::OnExit();
    }

    void OnInitCmdLine(wxCmdLineParser& parser) override
    {
        wxApp::OnInitCmdLine(parser);
        parser.AddOption("", "config", "keep settings in this file instead of the usual place "
                                       "(two Ragtimes on one computer, a test bench)",
                         wxCMD_LINE_VAL_STRING);
        parser.AddSwitch("", "version", "print the version and quit");
    }

    bool OnCmdLineParsed(wxCmdLineParser& parser) override
    {
        if (parser.Found("version"))
        {
            std::printf("Ragtime %s\n", ragtime::versionString());
            return false;
        }
        wxString path;
        if (parser.Found("config", &path))
        {
            delete wxConfigBase::Set(new wxFileConfig("Ragtime", wxEmptyString, path, wxEmptyString,
                                                      wxCONFIG_USE_LOCAL_FILE));
        }
        return wxApp::OnCmdLineParsed(parser);
    }
};

#ifdef __WXMSW__
wxIMPLEMENT_APP(RagtimeApp);
#else
wxIMPLEMENT_APP_NO_MAIN(RagtimeApp);

// --version is answered before wxEntry, because GTK needs a display to start
// and CI has none.
int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--version") == 0)
        {
            std::printf("Ragtime %s\n", ragtime::versionString());
            return 0;
        }
    }
    return wxEntry(argc, argv);
}
#endif
