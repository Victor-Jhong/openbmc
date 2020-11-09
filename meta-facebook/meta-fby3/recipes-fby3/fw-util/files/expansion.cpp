#include <iostream>
#include <cstdio>
#include <cstring>
#include <openbmc/pal.h>
#include "expansion.h"
#ifdef BIC_SUPPORT
#include <facebook/bic.h>

using namespace std;

void ExpansionBoard::ready()
{
  uint8_t config_status = 0;
  bool is_present = true;
  int ret = 0;

  switch(fw_comp) {
    case FW_CPLD:
    case FW_BIC:
    case FW_BIC_BOOTLOADER:
    case FW_BB_BIC:
    case FW_BB_BIC_BOOTLOADER:
    case FW_BB_CPLD:
      return;
  }

  ret = bic_is_m2_exp_prsnt(slot_id);
  if ( ret < 0 ) {
    throw "Failed to get the config from " + fru + ":" + board_name;
  }

  config_status = (uint8_t) ret;
  switch (fw_comp) {
    case FW_1OU_BIC:
    case FW_1OU_BIC_BOOTLOADER:
    case FW_1OU_CPLD:
      if ( (config_status & PRESENT_1OU) != PRESENT_1OU ) 
        is_present = false;
      break;
    case FW_2OU_BIC:
    case FW_2OU_BIC_BOOTLOADER:
    case FW_2OU_CPLD:
      if ( (config_status & PRESENT_2OU) != PRESENT_2OU )
        is_present = false;
      break;
    case FW_2OU_PESW:
    case FW_2OU_PESW_VR:
      if ( (config_status & PRESENT_2OU) != PRESENT_2OU ) {
        is_present = false;
      } else {
        uint8_t type = 0xff;
        if ( fby3_common_get_2ou_board_type(slot_id, &type) < 0 ) {
          throw string("Failed to get 2OU board type");
        } else if ( type != GPV3_MCHP_BOARD && type != GPV3_BRCM_BOARD ) {
          throw string("Not present");
        }
      }

      // PESW are present when the power is in S0 state
      if ( is_present == true && fw_comp != FW_2OU_PESW_VR ) {
        uint8_t pwr_sts = 0x0;
        ret = pal_get_server_power(slot_id, &pwr_sts);
        if ( ret < 0 || pwr_sts == 0 )
          throw string("DC-off");
      }
      break;
    default:
      break;
  }

  if ( is_present == false )
    throw board_name + " is empty";
}
#endif
