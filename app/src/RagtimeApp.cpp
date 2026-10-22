#include <cstdio>
#include <cstring>

#include <wx/wx.h>

#include "MainFrame.h"
#include "ragtime/Version.h"

class RagtimeApp : public wxApp
{
public:
    bool OnInit() override
    {
        if (!wxApp::OnInit())
            return false;

        SetAppName("Ragtime");
        auto* frame = new MainFrame();
        frame->Show();
        return true;
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
