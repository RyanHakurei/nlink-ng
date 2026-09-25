/* nlink-view used to send the screen from a USB callback. That callback
 * runs inside the calculator's USB handler, and waiting there locks the
 * keypad. Live view now uses the normal screenshot link instead.
 *
 * This build only removes a service left by an older copy, then exits.
 */

#include <libndls.h>

int main(void) {
  TI_NN_StopService(0x40F1u);
  TI_NN_StopService(0x7101u);
  show_msgbox("nlink-view",
              "This program is no longer used.\n\n"
              "Close it, then use Live view in nlink-ng.");
  return 0;
}
