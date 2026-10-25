// csb-3ds: replaces imgui/imguifilebrowser.cpp in the 3DS build.
// The desktop front end uses this ImGui file dialog from its menu bar, which never
// runs on 3DS (games load from csb.cfg and save via the in-game disk icon).
// The real file depends on POSIX directory APIs we don't need, so it is stubbed.
#include <functional>   // imguifilebrowser.h uses std::reference_wrapper without including it
#include <string>
#include <vector>
#include "imgui.h"
#include "imguifilebrowser.h"

namespace imgui_addons {

ImGuiFileBrowser::ImGuiFileBrowser() {}
ImGuiFileBrowser::~ImGuiFileBrowser() {}

bool ImGuiFileBrowser::showFileDialog(const std::string &, const DialogMode, const ImVec2 &,
                                      const std::string &)
{
  return false;
}

} // namespace imgui_addons
