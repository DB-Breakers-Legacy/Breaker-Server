#include "DX11OverlayInternal.h"
#include "FrameworkAddon.h"

void DrawClientPage()
{
    ImGui::TextUnformatted("LAN-only release");
    ImGui::Separator();
    ImGui::TextWrapped("Account login and hosted master services are removed from this package.");
}
