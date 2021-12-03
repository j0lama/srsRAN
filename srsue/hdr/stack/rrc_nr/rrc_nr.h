/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2021 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#ifndef SRSUE_RRC_NR_H
#define SRSUE_RRC_NR_H

#include "../rrc/rrc_cell.h"
#include "rrc_nr_config.h"
#include "srsran/adt/circular_map.h"
#include "srsran/asn1/rrc_nr.h"
#include "srsran/asn1/rrc_nr_utils.h"
#include "srsran/common/block_queue.h"
#include "srsran/common/buffer_pool.h"
#include "srsran/common/common_nr.h"
#include "srsran/common/stack_procedure.h"
#include "srsran/common/task_scheduler.h"
#include "srsran/interfaces/ue_interfaces.h"
#include "srsran/interfaces/ue_nr_interfaces.h"
#include "srsran/interfaces/ue_rrc_interfaces.h"
#include "srsue/hdr/stack/upper/gw.h"

namespace srsue {

class usim_interface_rrc_nr;
class pdcp_interface_rrc;
class rlc_interface_rrc;

struct rrc_nr_metrics_t {};

class rrc_nr final : public rrc_interface_phy_nr,
                     public rrc_interface_pdcp,
                     public rrc_interface_rlc,
                     public rrc_interface_mac,
                     public rrc_nr_interface_rrc,
                     public rrc_nr_interface_nas_5g,
                     public srsran::timer_callback
{
public:
  rrc_nr(srsran::task_sched_handle task_sched_);
  ~rrc_nr();

  int init(phy_interface_rrc_nr*       phy_,
           mac_interface_rrc_nr*       mac_,
           rlc_interface_rrc*          rlc_,
           pdcp_interface_rrc*         pdcp_,
           gw_interface_rrc*           gw_,
           rrc_eutra_interface_rrc_nr* rrc_eutra_,
           usim_interface_rrc_nr*      usim_,
           srsran::timer_handler*      timers_,
           stack_interface_rrc*        stack_,
           const rrc_nr_args_t&        args_);

  void stop();
  void init_core_less();

  void get_metrics(rrc_nr_metrics_t& m);

  // Timeout callback interface
  void timer_expired(uint32_t timeout_id) final;
  void srsran_rrc_log(const char* str);

  enum direction_t { Rx = 0, Tx };
  template <class T>
  void log_rrc_message(const std::string&           source,
                       direction_t                  dir,
                       const srsran::byte_buffer_t* pdu,
                       const T&                     msg,
                       const std::string&           msg_type);
  template <class T>
  void log_rrc_message(const std::string&  source,
                       direction_t         dir,
                       asn1::dyn_octstring oct,
                       const T&            msg,
                       const std::string&  msg_type);

  void run_tti(uint32_t tti);

  // PHY interface
  void in_sync() final;
  void out_of_sync() final;
  void cell_search_found_cell(const cell_search_result_t& result) final{};

  // RLC interface
  void max_retx_attempted() final;
  void protocol_failure() final;

  // MAC interface
  void ra_completed() final;
  void ra_problem() final;
  void release_pucch_srs() final;

  // PDCP interface
  void write_pdu(uint32_t lcid, srsran::unique_byte_buffer_t pdu) final;
  void write_pdu_bcch_bch(srsran::unique_byte_buffer_t pdu) final;
  void write_pdu_bcch_dlsch(srsran::unique_byte_buffer_t pdu) final;
  void write_pdu_pcch(srsran::unique_byte_buffer_t pdu) final;
  void write_pdu_mch(uint32_t lcid, srsran::unique_byte_buffer_t pdu) final;
  void notify_pdcp_integrity_error(uint32_t lcid) final;

  // NAS interface
  int      write_sdu(srsran::unique_byte_buffer_t sdu);
  bool     is_connected();
  int      connection_request(srsran::nr_establishment_cause_t cause, srsran::unique_byte_buffer_t sdu);
  uint16_t get_mcc();
  uint16_t get_mnc();

  // RRC (LTE) interface
  int  get_eutra_nr_capabilities(srsran::byte_buffer_t* eutra_nr_caps);
  int  get_nr_capabilities(srsran::byte_buffer_t* eutra_nr_caps);
  void phy_meas_stop();
  void phy_set_cells_to_meas(uint32_t carrier_freq_r15);
  bool rrc_reconfiguration(bool                endc_release_and_add_r15,
                           bool                nr_secondary_cell_group_cfg_r15_present,
                           asn1::dyn_octstring nr_secondary_cell_group_cfg_r15,
                           bool                sk_counter_r15_present,
                           uint32_t            sk_counter_r15,
                           bool                nr_radio_bearer_cfg1_r15_present,
                           asn1::dyn_octstring nr_radio_bearer_cfg1_r15);
  void rrc_release();
  bool configure_sk_counter(uint16_t sk_counter);
  bool is_config_pending();
  // STACK interface
  void cell_search_completed(const rrc_interface_phy_lte::cell_search_ret_t& cs_ret, const phy_cell_t& found_cell);

  void set_phy_config_complete(bool status) final;

private:
  // parsers
  void decode_pdu_bcch_dlsch(srsran::unique_byte_buffer_t pdu);
  // senders
  void send_setup_request(srsran::nr_establishment_cause_t cause);
  void send_ul_info_transfer(srsran::unique_byte_buffer_t nas_msg);
  void send_ul_ccch_msg(const asn1::rrc_nr::ul_ccch_msg_s& msg);
  // helpers
  void handle_sib1(const asn1::rrc_nr::sib1_s sib1);

  srsran::task_sched_handle task_sched;
  struct cmd_msg_t {
    enum { PDU, PCCH, PDU_MCH, RLF, PDU_BCCH_DLSCH, STOP } command;
    srsran::unique_byte_buffer_t pdu;
    uint16_t                     lcid;
  };

  srslog::basic_logger&          logger;
  bool                           running = false;
  srsran::block_queue<cmd_msg_t> cmd_q;

  // PHY config
  srsran::phy_cfg_nr_t phy_cfg = {};

  phy_interface_rrc_nr*       phy       = nullptr;
  mac_interface_rrc_nr*       mac       = nullptr;
  rlc_interface_rrc*          rlc       = nullptr;
  pdcp_interface_rrc*         pdcp      = nullptr;
  gw_interface_rrc*           gw        = nullptr;
  rrc_eutra_interface_rrc_nr* rrc_eutra = nullptr;
  usim_interface_rrc_nr*      usim      = nullptr;
  stack_interface_rrc*        stack     = nullptr;

  meas_cell_list<meas_cell_nr> meas_cells;

  // PLMN
  bool                         plmn_is_selected = false;
  srsran::unique_byte_buffer_t dedicated_info_nas;

  const uint32_t                      sim_measurement_timer_duration_ms = 250;
  uint32_t                            sim_measurement_carrier_freq_r15;
  srsran::timer_handler::unique_timer sim_measurement_timer;

  /// RRC states (3GPP 38.331 v15.5.1 Sec 4.2.1)
  enum rrc_nr_state_t {
    RRC_NR_STATE_IDLE = 0,
    RRC_NR_STATE_CONNECTED,
    RRC_NR_STATE_CONNECTED_INACTIVE,
    RRC_NR_STATE_N_ITEMS,
  };
  const static char* rrc_nr_state_text[RRC_NR_STATE_N_ITEMS];
  rrc_nr_state_t     state = RRC_NR_STATE_IDLE;

  // Stores the state of the PHY configuration setting
  enum {
    PHY_CFG_STATE_NONE = 0,
    PHY_CFG_STATE_APPLY_SP_CELL,
    PHY_CFG_STATE_RA_COMPLETED,
  } phy_cfg_state;

  rrc_nr_args_t args = {};

  const char* get_rb_name(uint32_t lcid) final;

  bool     add_lcid_drb(uint32_t lcid, uint32_t drb_id);
  uint32_t get_lcid_for_drbid(uint32_t rdid);

  std::map<uint32_t, srsran::nr_drb> lcid_drb; // Map of lcid to drb

  std::map<uint32_t, uint32_t> drb_eps_bearer_id; // Map of drb id to eps_bearer_id

  // temporary maps for building the pucch nr resources
  srsran::static_circular_map<uint32_t, srsran_pucch_nr_resource_t, 128> pucch_res_list;
  std::map<uint32_t, srsran_csi_rs_zp_resource_t>                        csi_rs_zp_res;
  std::map<uint32_t, srsran_csi_rs_nzp_resource_t>                       csi_rs_nzp_res;

  bool apply_cell_group_cfg(const asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg);
  bool apply_radio_bearer_cfg(const asn1::rrc_nr::radio_bearer_cfg_s& radio_bearer_cfg);
  bool apply_rlc_add_mod(const asn1::rrc_nr::rlc_bearer_cfg_s& rlc_bearer_cfg);
  bool apply_mac_cell_group(const asn1::rrc_nr::mac_cell_group_cfg_s& mac_cell_group_cfg);
  bool apply_sp_cell_cfg(const asn1::rrc_nr::sp_cell_cfg_s& sp_cell_cfg);
  bool apply_phy_cell_group_cfg(const asn1::rrc_nr::phys_cell_group_cfg_s& phys_cell_group_cfg);
  bool apply_dl_common_cfg(const asn1::rrc_nr::dl_cfg_common_s& dl_cfg_common);
  bool apply_ul_common_cfg(const asn1::rrc_nr::ul_cfg_common_s& ul_cfg_common);
  bool apply_sp_cell_init_dl_pdcch(const asn1::rrc_nr::pdcch_cfg_s& pdcch_cfg);
  bool apply_sp_cell_init_dl_pdsch(const asn1::rrc_nr::pdsch_cfg_s& pdsch_cfg);
  bool apply_sp_cell_ded_ul_pucch(const asn1::rrc_nr::pucch_cfg_s& pucch_cfg);
  bool apply_sp_cell_ded_ul_pusch(const asn1::rrc_nr::pusch_cfg_s& pusch_cfg);
  bool apply_csi_meas_cfg(const asn1::rrc_nr::csi_meas_cfg_s& csi_meas_cfg);
  bool apply_res_csi_report_cfg(const asn1::rrc_nr::csi_report_cfg_s& csi_report_cfg);
  bool apply_drb_add_mod(const asn1::rrc_nr::drb_to_add_mod_s& drb_cfg);
  bool apply_drb_release(const uint8_t drb);
  bool apply_security_cfg(const asn1::rrc_nr::security_cfg_s& security_cfg);

  srsran::as_security_config_t sec_cfg;

  typedef enum { mcg_srb1, en_dc_srb3, nr } reconf_initiator_t;

  // RRC procedures
  enum class cell_search_result_t { changed_cell, same_cell, no_cell };
  class cell_selection_proc;
  class connection_reconf_no_ho_proc;
  class setup_request_proc;

  srsran::proc_t<cell_selection_proc, cell_search_result_t> cell_selector;
  srsran::proc_t<connection_reconf_no_ho_proc>              conn_recfg_proc;
  srsran::proc_t<setup_request_proc>                        setup_req_proc;

  srsran::proc_manager_list_t callback_list;
};

} // namespace srsue

#endif // SRSUE_RRC_NR_H
