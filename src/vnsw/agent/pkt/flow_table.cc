/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <vector>
#include <bitset>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <sandesh/sandesh_types.h>
#include <sandesh/sandesh.h>
#include <sandesh/sandesh_trace.h>
#include <pkt/flow_table.h>
#include <uve/flow_stats_collector.h>
#include <ksync/flowtable_ksync.h>
#include <ksync/ksync_init.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

#include "route/route.h"
#include "cmn/agent_cmn.h"
#include "oper/interface_common.h"
#include "oper/nexthop.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

#include "route/route.h"
#include "init/agent_param.h"
#include "cmn/agent_cmn.h"
#include "cmn/agent_stats.h"
#include "oper/interface_common.h"
#include "oper/nexthop.h"
#include "oper/route_common.h"
#include "oper/vrf.h"
#include "oper/vm.h"
#include "oper/sg.h"

#include "filter/packet_header.h"
#include "filter/acl.h"

#include "pkt/proto.h"
#include "pkt/proto_handler.h"
#include "pkt/pkt_handler.h"
#include "pkt/flow_proto.h"
#include "pkt/pkt_types.h"
#include "uve/agent_uve.h"
#include "pkt/pkt_sandesh_flow.h"

boost::uuids::random_generator FlowTable::rand_gen_ = boost::uuids::random_generator();
tbb::atomic<int> FlowEntry::alloc_count_;

static bool ShouldDrop(uint32_t action) {
    if ((action & TrafficAction::DROP_FLAGS) || (action & TrafficAction::IMPLICIT_DENY_FLAGS))
        return true;

    return false;
}

static void SetAclListAceId(const AclDBEntry *acl, const std::list<MatchAclParams> &acl_l,
                            std::vector<AceId> &ace_l) {
    std::list<MatchAclParams>::const_iterator ma_it;
    for (ma_it = acl_l.begin();
         ma_it != acl_l.end();
         ++ma_it) {
        if ((*ma_it).acl != acl) {
            continue;
        }
        AclEntryIDList::const_iterator ait;
        for (ait = (*ma_it).ace_id_list.begin(); 
             ait != (*ma_it).ace_id_list.end(); ++ ait) {
            AceId ace_id;
            ace_id.id = *ait;
            ace_l.push_back(ace_id);
        }
    }
}

FlowEntry::FlowEntry(const FlowKey &k) : 
    key_(k), data_(), stats_(), flow_handle_(kInvalidFlowHandle),
    ksync_entry_(NULL), deleted_(false), flags_(0), linklocal_src_port_(),
    linklocal_src_port_fd_(PktFlowInfo::kLinkLocalInvalidFd) {
    flow_uuid_ = FlowTable::rand_gen_(); 
    egress_uuid_ = FlowTable::rand_gen_(); 
    refcount_ = 0;
    alloc_count_.fetch_and_increment();
}

uint32_t FlowEntry::MatchAcl(const PacketHeader &hdr,
                             std::list<MatchAclParams> &acl,
                             bool add_implicit_deny, bool add_implicit_allow) {
    // If there are no ACL to match, make it pass
    if (acl.size() == 0 &&  add_implicit_allow) {
        return (1 << TrafficAction::PASS);
    }

    // PASS default GW traffic, if it is ICMP or DNS
    if ((hdr.protocol == IPPROTO_ICMP ||
         (hdr.protocol == IPPROTO_UDP && 
          (hdr.src_port == DNS_SERVER_PORT || hdr.dst_port == DNS_SERVER_PORT))) &&
        (Agent::GetInstance()->pkt()->pkt_handler()->
         IsGwPacket(data_.intf_entry.get(), hdr.dst_ip) ||
         Agent::GetInstance()->pkt()->pkt_handler()->
         IsGwPacket(data_.intf_entry.get(), hdr.src_ip))) {
        return (1 << TrafficAction::PASS);
    }

    uint32_t action = 0;
    for (std::list<MatchAclParams>::iterator it = acl.begin();
         it != acl.end(); ++it) {
        if (it->acl.get() == NULL) {
            continue;
        }

        if (it->acl->PacketMatch(hdr, *it)) {
            action |= it->action_info.action;
            if (it->action_info.action & (1 << TrafficAction::MIRROR)) {
                data_.match_p.action_info.mirror_l.insert
                    (data_.match_p.action_info.mirror_l.end(),
                     it->action_info.mirror_l.begin(),
                     it->action_info.mirror_l.end());
            }

            //If vrf translate action, update action info
            if (it->action_info.action & (1 << TrafficAction::VRF_TRANSLATE)) {
                std::string vrf =
                    it->action_info.vrf_translate_action_.vrf_name();
                data_.match_p.action_info.vrf_translate_action_.set_vrf_name(vrf);
                //Check if VRF assign acl says, network ACL and SG action
                //to be ignored
                bool ignore_acl =
                    it->action_info.vrf_translate_action_.ignore_acl();
                data_.match_p.action_info.vrf_translate_action_.
                    set_ignore_acl(ignore_acl);
            }

            if (it->terminal_rule) {
                break;
            }
        }
    }

    // If no acl matched, make it imlicit deny
    if (action == 0 && add_implicit_deny) {
        action = (1 << TrafficAction::DROP) | 
            (1 << TrafficAction::IMPLICIT_DENY);;
    }

    return action;
}

void FlowEntry::ResetStats() {
    stats_.bytes = 0;
    stats_.packets = 0;
}

// Recompute FlowEntry action
bool FlowEntry::ActionRecompute() {
    uint32_t action = 0;

    action = data_.match_p.policy_action | data_.match_p.out_policy_action |
        data_.match_p.sg_action_summary |
        data_.match_p.mirror_action | data_.match_p.out_mirror_action |
        data_.match_p.vrf_assign_acl_action;

    if (action & (1 << TrafficAction::VRF_TRANSLATE) && 
        data_.match_p.action_info.vrf_translate_action_.ignore_acl() == true) {
        action = (1 << TrafficAction::VRF_TRANSLATE) |
                 (1 << TrafficAction::PASS);
    }

    // Force short flows to DROP
    if (is_flags_set(FlowEntry::ShortFlow)) {
        action |= (1 << TrafficAction::DROP);
    }

    // check for conflicting actions and remove allowed action
    if (ShouldDrop(action)) {
        action = (action & ~TrafficAction::DROP_FLAGS & ~TrafficAction::PASS_FLAGS);
        action |= (1 << TrafficAction::DROP);
    }

    if (action & (1 << TrafficAction::TRAP)) {
        action = (1 << TrafficAction::TRAP);
    }

    if (action != data_.match_p.action_info.action) {
        data_.match_p.action_info.action = action;
        return true;
    }

    return false;
}

void FlowEntry::SetPacketHeader(PacketHeader *hdr) {
    hdr->vrf = key_.vrf;
    hdr->src_ip = key_.src.ipv4;
    hdr->dst_ip = key_.dst.ipv4;
    hdr->protocol = key_.protocol;
    if (hdr->protocol == IPPROTO_UDP || hdr->protocol == IPPROTO_TCP) {
        hdr->src_port = key_.src_port;
        hdr->dst_port = key_.dst_port;
    } else {
        hdr->src_port = 0;
        hdr->dst_port = 0;
    }
    hdr->src_policy_id = &(data_.source_vn);
    hdr->dst_policy_id = &(data_.dest_vn);
    hdr->src_sg_id_l = &(data_.source_sg_id_l);
    hdr->dst_sg_id_l = &(data_.dest_sg_id_l);
}


// In case of NAT flows, the key fields can change.
void FlowEntry::SetOutPacketHeader(PacketHeader *hdr) {
    FlowEntry *rflow = reverse_flow_entry();
    if (rflow == NULL)
        return;

    hdr->vrf = rflow->key().vrf;
    hdr->src_ip = rflow->key().dst.ipv4;
    hdr->dst_ip = rflow->key().src.ipv4;
    hdr->protocol = rflow->key().protocol;
    if (hdr->protocol == IPPROTO_UDP || hdr->protocol == IPPROTO_TCP) {
        hdr->src_port = rflow->key().dst_port;
        hdr->dst_port = rflow->key().src_port;
    } else {
        hdr->src_port = 0;
        hdr->dst_port = 0;
    }
    hdr->src_policy_id = &(rflow->data().dest_vn);
    hdr->dst_policy_id = &(rflow->data().source_vn);
    hdr->src_sg_id_l = &(rflow->data().dest_sg_id_l);
    hdr->dst_sg_id_l = &(rflow->data().source_sg_id_l);
}

// Apply Policy and SG rules for a flow.
//
// Special case of local flows:
//     For local-flows, both VM are on same compute and we need to apply SG from
//     both the ports. m_sg_acl_l will contain ACL for port in forward flow and
//     m_out_sg_acl_l will have ACL from other port
//
//     If forward flow goes thru NAT, the key for matching ACL in 
//     m_out_sg_acl_l can potentially change. The routine SetOutPacketHeader
//     takes care of forming header after NAT
//
// Rules applied are based on flow type
// Non-Local Forward Flow
//      Network Policy. 
//      Out-Network Policy will be empty
//      SG
//      Out-SG will be empty
// Non-Local Reverse Flow
//      Network Policy. 
//      Out-Network Policy will be empty
//      SG and out-SG from forward flow
// Local Forward Flow
//      Network Policy. 
//      Out-Network Policy
//      SG
//      Out-SG 
// Local Reverse Flow
//      Network Policy. 
//      Out-Network Policy
//      SG and out-SG from forward flow
bool FlowEntry::DoPolicy() {
    data_.match_p.action_info.Clear();
    data_.match_p.policy_action = 0;
    data_.match_p.out_policy_action = 0;
    data_.match_p.sg_action = 0;
    data_.match_p.out_sg_action = 0;
    data_.match_p.reverse_sg_action = 0;
    data_.match_p.reverse_out_sg_action = 0;
    data_.match_p.mirror_action = 0;
    data_.match_p.out_mirror_action = 0;
    data_.match_p.sg_action_summary = 0;

    FlowEntry *rflow = reverse_flow_entry();
    PacketHeader hdr;
    SetPacketHeader(&hdr);

    //Calculate VRF assign entry, and ignore acl is set
    //skip network and SG acl action is set
    data_.match_p.vrf_assign_acl_action =
        MatchAcl(hdr, data_.match_p.m_vrf_assign_acl_l, false, true);
    if (data_.match_p.vrf_assign_acl_action & 
        (1 << TrafficAction::VRF_TRANSLATE) && acl_assigned_vrf_index() == 0) {
         MakeShortFlow();
    }

    // Mirror is valid even if packet is to be dropped. So, apply it first
    data_.match_p.mirror_action = MatchAcl(hdr, data_.match_p.m_mirror_acl_l,
                                           false, true);

    // Apply out-policy. Valid only for local-flow
    data_.match_p.out_mirror_action = MatchAcl(hdr,
                           data_.match_p.m_out_mirror_acl_l, false, true);

    // Apply network policy
    data_.match_p.policy_action = MatchAcl(hdr, data_.match_p.m_acl_l, true,
                                           true);
    if (ShouldDrop(data_.match_p.policy_action)) {
        goto done;
    }

    data_.match_p.out_policy_action = MatchAcl(hdr, data_.match_p.m_out_acl_l,
                                               true, true);
    if (ShouldDrop(data_.match_p.out_policy_action)) {
        goto done;
    }

    // Apply security-group
    if (!is_flags_set(FlowEntry::ReverseFlow)) {
        data_.match_p.sg_action = MatchAcl(hdr, data_.match_p.m_sg_acl_l, true,
                                           !data_.match_p.sg_rule_present);

        PacketHeader out_hdr;
        if (ShouldDrop(data_.match_p.sg_action) == false && rflow) {
            // Key fields for lookup in out-acl can potentially change in case 
            // of NAT. Form ACL lookup based on post-NAT fields
            SetOutPacketHeader(&out_hdr);
            data_.match_p.out_sg_action =
                MatchAcl(out_hdr, data_.match_p.m_out_sg_acl_l, true,
                         !data_.match_p.out_sg_rule_present);
        }

        // For TCP-ACK packet, we allow packet if either forward or reverse
        // flow says allow. So, continue matching reverse flow even if forward
        // flow says drop
        if (is_flags_set(FlowEntry::TcpAckFlow) && rflow) {
            rflow->SetPacketHeader(&hdr);
            data_.match_p.reverse_sg_action =
                MatchAcl(hdr, data_.match_p.m_reverse_sg_acl_l, true,
                         !data_.match_p.reverse_sg_rule_present);
            if (ShouldDrop(data_.match_p.reverse_sg_action) == false) {
                // Key fields for lookup in out-acl can potentially change in
                // case of NAT. Form ACL lookup based on post-NAT fields
                rflow->SetOutPacketHeader(&out_hdr);
                data_.match_p.reverse_out_sg_action =
                    MatchAcl(out_hdr, data_.match_p.m_reverse_out_sg_acl_l, true,
                             !data_.match_p.reverse_out_sg_rule_present);
            }
        }

        // Compute summary SG action.
        // For Non-TCP-ACK Flows
        //     DROP if any of sg_action, sg_out_action, reverse_sg_action or
        //     reverse_out_sg_action says DROP
        // For TCP-ACK flows
        //     ALLOW if either ((sg_action && sg_out_action) ||
        //                      (reverse_sg_action & reverse_out_sg_action)) ALLOW
        data_.match_p.sg_action_summary = 0;
        if (!is_flags_set(FlowEntry::TcpAckFlow)) {
            data_.match_p.sg_action_summary =
                data_.match_p.sg_action |
                data_.match_p.out_sg_action |
                data_.match_p.reverse_sg_action |
                data_.match_p.reverse_out_sg_action;
        } else {
            if (ShouldDrop(data_.match_p.sg_action |
                           data_.match_p.out_sg_action)
                &&
                ShouldDrop(data_.match_p.reverse_sg_action |
                           data_.match_p.reverse_out_sg_action)) {
                data_.match_p.sg_action_summary = (1 << TrafficAction::DROP);
            } else {
                data_.match_p.sg_action_summary = (1 << TrafficAction::PASS);
            }
        }
    } else {
        // SG is reflexive ACL. For reverse-flow, copy SG action from
        // forward flow 
        UpdateReflexiveAction();
    }

done:
    // Set mirror vrf after evaluation of actions
    SetMirrorVrfFromAction();
    // Summarize the actions based on lookups above
    ActionRecompute();
    return true;
}

// SetMirrorVrfFromAction
// For this flow check for mirror action from dynamic ACLs or policy mirroring
// assign the vrf from its Virtual Nework that ACL is used
// If it is a local flow and out mirror action or policy is set
// assign the vrf of the reverse flow, since ACL came from the reverse flow
void FlowEntry::SetMirrorVrfFromAction() {
    if (data_.match_p.mirror_action & (1 << TrafficAction::MIRROR) ||
        data_.match_p.policy_action & (1 << TrafficAction::MIRROR)) {
        const VnEntry *vn = vn_entry();
        if (vn && vn->GetVrf()) {
            SetMirrorVrf(vn->GetVrf()->vrf_id());
        }
    }
    if (data_.match_p.out_mirror_action & (1 << TrafficAction::MIRROR) ||
        data_.match_p.out_policy_action & (1 << TrafficAction::MIRROR)) {
        FlowEntry *rflow = reverse_flow_entry_.get();
        if (rflow) {
            const VnEntry *rvn = rflow->vn_entry();
            if (rvn && rvn->GetVrf()) {
                SetMirrorVrf(rvn->GetVrf()->vrf_id());
            }
        }
    }
}

// Ingress-ACL/Egress-ACL in interface with VM as reference point.
//      Ingress : Packet to VM
//      Egress  : Packet from VM
// The direction stored in flow is defined with vrouter as reference point
//      Ingress : Packet to Vrouter from VM
//      Egress  : Packet from Vrouter to VM
// 
// Function takes care of copying right rules
static bool CopySgEntries(const VmInterface *vm_port, bool ingress_acl,
                          std::list<MatchAclParams> &list) {
    bool ret = false;
    for (VmInterface::SecurityGroupEntrySet::const_iterator it =
         vm_port->sg_list().list_.begin();
         it != vm_port->sg_list().list_.end(); ++it) {
        if (it->sg_->IsAclSet()) {
            ret = true;
        }
        MatchAclParams acl;
        // As per definition above, 
        //      get EgressACL if flow direction is Ingress
        //      get IngressACL if flow direction is Egress
        if (ingress_acl) {
            acl.acl = it->sg_->GetEgressAcl();
        } else {
            acl.acl = it->sg_->GetIngressAcl();
        }
        if (acl.acl)
            list.push_back(acl);
    }

    return ret;
}

void FlowEntry::GetLocalFlowSgList(const VmInterface *vm_port,
                                   const VmInterface *reverse_vm_port) {
    // Get SG-Rule for the forward flow
    data_.match_p.sg_rule_present = CopySgEntries(vm_port, true,
                                                  data_.match_p.m_sg_acl_l);
    // For local flow, we need to simulate SG lookup at both ends.
    // Assume packet is from VM-A to VM-B.
    // If we apply Ingress-ACL from VM-A, then apply Egress-ACL from VM-B
    // If we apply Egress-ACL from VM-A, then apply Ingress-ACL from VM-B
    if (reverse_vm_port) {
        data_.match_p.out_sg_rule_present =
            CopySgEntries(reverse_vm_port, false, data_.match_p.m_out_sg_acl_l);
    }

    if (!is_flags_set(FlowEntry::TcpAckFlow)) {
        return;
    }

    // TCP ACK workaround:
    // Ideally TCP State machine should be run to age TCP flows
    // Temporary workaound in place of state machine. For TCP ACK packets allow
    // the flow if either forward or reverse flow is allowed

    // Copy the SG rules to be applied for reverse flow
    data_.match_p.reverse_out_sg_rule_present =
        CopySgEntries(vm_port, false,
                      data_.match_p.m_reverse_out_sg_acl_l);

    if (reverse_vm_port) {
        data_.match_p.reverse_sg_rule_present =
            CopySgEntries(reverse_vm_port, true,
                          data_.match_p.m_reverse_sg_acl_l);
    }
}

void FlowEntry::GetNonLocalFlowSgList(const VmInterface *vm_port) {
    // Get SG-Rule for the forward flow
    bool ingress = is_flags_set(FlowEntry::IngressDir);
    data_.match_p.sg_rule_present = CopySgEntries(vm_port, ingress,
                                                  data_.match_p.m_sg_acl_l);
    data_.match_p.out_sg_rule_present = false;

    if (!is_flags_set(FlowEntry::TcpAckFlow)) {
        return;
    }

    // TCP ACK workaround:
    // Ideally TCP State machine should be run to age TCP flows
    // Temporary workaound in place of state machine. For TCP ACK packets allow
    // the flow if either forward or reverse flow is allowed

    // Copy the SG rules to be applied for reverse flow
    data_.match_p.reverse_out_sg_rule_present =
        CopySgEntries(vm_port, !ingress,
                      data_.match_p.m_reverse_out_sg_acl_l);
    data_.match_p.reverse_sg_rule_present = false;
}

void FlowEntry::GetSgList(const Interface *intf) {
    // Dont apply network-policy for linklocal and multicast flows
    if (is_flags_set(FlowEntry::LinkLocalFlow) ||
        is_flags_set(FlowEntry::Multicast)) {
        return;
    }

    // SG ACL's are reflexive. Skip SG for reverse flow
    if (is_flags_set(FlowEntry::ReverseFlow)) {
        return;
    }

    // Get virtual-machine port for forward flow
    const VmInterface *vm_port = NULL;
    if (intf != NULL) {
        if (intf->type() == Interface::VM_INTERFACE) {
            vm_port = static_cast<const VmInterface *>(intf);
         }
    }

    if (vm_port == NULL) {
        return;
    }

    // Get virtual-machine port for reverse flow
    FlowEntry *rflow = reverse_flow_entry();
    const VmInterface *reverse_vm_port = NULL;
    if (rflow != NULL) {
        if (rflow->data().intf_entry.get() != NULL) {
            if (rflow->data().intf_entry->type() == Interface::VM_INTERFACE) {
                reverse_vm_port = static_cast<const VmInterface *>
                    (rflow->data().intf_entry.get());
            }
        }
    }

    // Get SG-Rules
    if (is_flags_set(FlowEntry::LocalFlow)) {
        GetLocalFlowSgList(vm_port, reverse_vm_port);
    } else {
        GetNonLocalFlowSgList(vm_port);
    }
}

void FlowEntry::ResetPolicy() {
    /* Reset acl list*/
    data_.match_p.m_acl_l.clear();
    data_.match_p.m_out_acl_l.clear();
    data_.match_p.m_mirror_acl_l.clear();
    data_.match_p.m_out_mirror_acl_l.clear();
    /* Reset sg acl list*/
    data_.match_p.sg_rule_present = false;
    data_.match_p.m_sg_acl_l.clear();
    data_.match_p.out_sg_rule_present = false;
    data_.match_p.m_out_sg_acl_l.clear();

    data_.match_p.reverse_sg_rule_present = false;
    data_.match_p.m_reverse_sg_acl_l.clear();
    data_.match_p.reverse_out_sg_rule_present = false;
    data_.match_p.m_reverse_out_sg_acl_l.clear();
    data_.match_p.m_vrf_assign_acl_l.clear();
}

void FlowEntry::GetPolicy(const VnEntry *vn) {
    if (vn == NULL)
        return;

    MatchAclParams acl;

    // Get Mirror configuration first
    if (vn->GetMirrorAcl()) {
        acl.acl = vn->GetMirrorAcl();
        data_.match_p.m_mirror_acl_l.push_back(acl);
    }

    if (vn->GetMirrorCfgAcl()) {
        acl.acl = vn->GetMirrorCfgAcl();
        data_.match_p.m_mirror_acl_l.push_back(acl);
    }

    // Dont apply network-policy for linklocal and subnet broadcast flow
    if (is_flags_set(FlowEntry::LinkLocalFlow) ||
        is_flags_set(FlowEntry::Multicast)) {
        return;
    }

    if (vn->GetAcl()) {
        acl.acl = vn->GetAcl();
        data_.match_p.m_acl_l.push_back(acl);
    }

    const VnEntry *rvn = NULL;
    FlowEntry *rflow = reverse_flow_entry_.get();
    // For local flows, we have to apply NW Policy from out-vn also
    if (!is_flags_set(FlowEntry::LocalFlow) || rflow == NULL) {
        // Not local flow
        return;
    }

    rvn = rflow->vn_entry();
    if (rvn == NULL) {
        return;
    }

    if (rvn->GetAcl()) {
        acl.acl = rvn->GetAcl();
        data_.match_p.m_out_acl_l.push_back(acl);
    }

    if (rvn->GetMirrorAcl()) {
        acl.acl = rvn->GetMirrorAcl();
        data_.match_p.m_out_mirror_acl_l.push_back(acl);
    }

    if (rvn->GetMirrorCfgAcl()) {
        acl.acl = rvn->GetMirrorCfgAcl();
        data_.match_p.m_out_mirror_acl_l.push_back(acl);
    }
}

void FlowEntry::GetVrfAssignAcl() {
    if (data_.intf_entry == NULL) {
        return;
    }

    if  (data_.intf_entry->type() != Interface::VM_INTERFACE) {
        return;
    }

    if (is_flags_set(FlowEntry::LinkLocalFlow) ||
        is_flags_set(FlowEntry::Multicast)) {
        return;
    }

    const VmInterface *intf =
        static_cast<const VmInterface *>(data_.intf_entry.get());
    //If interface has a VRF assign rule, choose the acl and match the
    //packet, else get the acl attached to VN and try matching the packet to
    //network acl
    const AclDBEntry* acl = intf->vrf_assign_acl();
    if (acl == NULL &&  data_.vn_entry.get() != NULL) {
        acl = data_.vn_entry.get()->GetAcl();
    }
    if (!acl) {
        return;
    }

    MatchAclParams m_acl;
    m_acl.acl = acl;
    data_.match_p.m_vrf_assign_acl_l.push_back(m_acl);
}

const std::string& FlowEntry::acl_assigned_vrf() const {
    return data_.match_p.action_info.vrf_translate_action_.vrf_name();
}

uint32_t FlowEntry::acl_assigned_vrf_index() const {
    VrfKey vrf_key(data_.match_p.action_info.vrf_translate_action_.vrf_name());
    const VrfEntry *vrf = static_cast<const VrfEntry *>(
            Agent::GetInstance()->GetVrfTable()->FindActiveEntry(&vrf_key));
    if (vrf) {
        return vrf->vrf_id();
    }
    return 0;
}

void FlowEntry::UpdateKSync() {
    FlowInfo flow_info;
    FillFlowInfo(flow_info);
    if (stats_.last_modified_time != stats_.setup_time) {
        /*
         * Do not export stats on flow creation, it will be exported
         * while updating stats
         */
        FlowStatsCollector::FlowExport(this, 0, 0);
    }
    FlowTableKSyncObject *ksync_obj = 
        Agent::GetInstance()->ksync()->flowtable_ksync_obj();

    if (ksync_entry_ == NULL) {
        FLOW_TRACE(Trace, "Add", flow_info);
        FlowTableKSyncEntry key(ksync_obj, this, flow_handle_);
        ksync_entry_ =
            static_cast<FlowTableKSyncEntry *>(ksync_obj->Create(&key));
    } else {
        FLOW_TRACE(Trace, "Change", flow_info);
        ksync_obj->Change(ksync_entry_);    
    }
}

void FlowEntry::MakeShortFlow() {
    set_flags(FlowEntry::ShortFlow);
    if (reverse_flow_entry_) {
        reverse_flow_entry_->set_flags(FlowEntry::ShortFlow);
    }
}

void FlowEntry::GetPolicyInfo(const VnEntry *vn) {
    // Default make it false
    ResetPolicy();

    // Short flows means there is some information missing for the flow. Skip 
    // getting policy information for short flow. When the information is
    // complete, GetPolicyInfo is called again
    if (is_flags_set(FlowEntry::ShortFlow)) {
        return;
    }

    // ACL supported on VMPORT interfaces only
    if (data_.intf_entry == NULL)
        return;

    if  (data_.intf_entry->type() != Interface::VM_INTERFACE)
        return;

    // Get Network policy/mirror cfg policy/mirror policies 
    GetPolicy(vn);

    // Get Sg list
    GetSgList(data_.intf_entry.get());

    //Get VRF translate ACL
    GetVrfAssignAcl();
}

void FlowEntry::GetPolicyInfo() {
    GetPolicyInfo(data_.vn_entry.get());
}

void FlowTable::Add(FlowEntry *flow, FlowEntry *rflow) {
    flow->reset_flags(FlowEntry::ReverseFlow);
    /* reverse flow may not be aviable always, eg: Flow Audit */
    if (rflow != NULL)
        rflow->set_flags(FlowEntry::ReverseFlow);
    UpdateReverseFlow(flow, rflow);

    flow->GetPolicyInfo();
    // Add the forward flow after adding the reverse flow first to avoid 
    // following sequence
    // 1. Agent adds forward flow
    // 2. vrouter releases the packet
    // 3. Packet reaches destination VM and destination VM replies
    // 4. Agent tries adding reverse flow. vrouter processes request in core-0
    // 5. vrouter gets reverse packet in core-1
    // 6. If (4) and (3) happen together, vrouter can allocate 2 hash entries
    //    for the flow.
    //
    // While the scenario above cannot be totally avoided, programming reverse
    // flow first will reduce the probability

    if (rflow) {
        rflow->GetPolicyInfo();
        ResyncAFlow(rflow);
        AddFlowInfo(rflow);
    }


    ResyncAFlow(flow);
    AddFlowInfo(flow);
}

void FlowTable::UpdateReverseFlow(FlowEntry *flow, FlowEntry *rflow) {
    FlowEntry *flow_rev = flow->reverse_flow_entry();
    FlowEntry *rflow_rev = NULL;

    if (rflow) {
        rflow_rev = rflow->reverse_flow_entry();
    }

    if (rflow_rev) {
        assert(rflow_rev->reverse_flow_entry() == rflow);
        rflow_rev->set_reverse_flow_entry(NULL);
    }

    if (flow_rev) {
        flow_rev->set_reverse_flow_entry(NULL);
    }

    flow->set_reverse_flow_entry(rflow);
    if (rflow) {
        rflow->set_reverse_flow_entry(flow);
    }

    if (flow_rev && (flow_rev->reverse_flow_entry() == NULL)) {
        flow_rev->MakeShortFlow();
        flow->MakeShortFlow();
    }

    if (rflow_rev && (rflow_rev->reverse_flow_entry() == NULL)) {
        rflow_rev->MakeShortFlow();
        flow->MakeShortFlow();
    }

    if (flow->reverse_flow_entry() == NULL) {
        flow->MakeShortFlow();
    }

    if (rflow && rflow->reverse_flow_entry() == NULL) {
        rflow->MakeShortFlow();
    }

    if (rflow) {
        if (flow->is_flags_set(FlowEntry::ShortFlow) ||
            rflow->is_flags_set(FlowEntry::ShortFlow)) {
            flow->MakeShortFlow();
        }
        if (flow->is_flags_set(FlowEntry::Multicast)) {
            rflow->set_flags(FlowEntry::Multicast);
        }
    }
}

void FlowEntry::UpdateFipStatsInfo(uint32_t fip, uint32_t id) {
    stats_.fip = fip;
    stats_.fip_vm_port_id = id;
}

void FlowEntry::FillFlowInfo(FlowInfo &info) {
    info.set_flow_index(flow_handle_);
    info.set_source_ip(Ip4Address(key_.src.ipv4).to_string());
    info.set_source_port(key_.src_port);
    info.set_destination_ip(Ip4Address(key_.dst.ipv4).to_string());
    info.set_destination_port(key_.dst_port);
    info.set_protocol(key_.protocol);
    info.set_vrf(key_.vrf);
    info.set_source_vn(data_.source_vn);
    info.set_dest_vn(data_.dest_vn);
    std::vector<uint32_t> v;
    SecurityGroupList::const_iterator it;
    for (it = data_.source_sg_id_l.begin();
            it != data_.source_sg_id_l.end(); it++) {
        v.push_back(*it);
    }
    info.set_source_sg_id_l(v);
    v.clear();
    for (it = data_.dest_sg_id_l.begin(); it != data_.dest_sg_id_l.end(); it++) {
        v.push_back(*it);
    }
    info.set_dest_sg_id_l(v);

    uint32_t fe_action = data_.match_p.action_info.action;
    if (fe_action & (1 << TrafficAction::DENY)) {
        info.set_deny(true);
    } else if (fe_action & (1 << TrafficAction::PASS)) {
        info.set_allow(true);
    }

    if (is_flags_set(FlowEntry::NatFlow)) {
        info.set_nat(true);
        FlowEntry *nat_flow = reverse_flow_entry_.get();
        if (nat_flow) {
            if (key_.src.ipv4 != nat_flow->key().dst.ipv4) {
                info.set_nat_source_ip(Ip4Address(nat_flow->key().dst.ipv4).\
                                       to_string());
            }

            if (key_.dst.ipv4 != nat_flow->key().src.ipv4) {
                info.set_nat_destination_ip(Ip4Address(nat_flow->key().src.ipv4).\
                                            to_string());
            }

            if (key_.src_port != nat_flow->key().dst_port)  {
                info.set_nat_source_port(nat_flow->key().dst_port);
            }

            if (key_.dst_port != nat_flow->key().src_port) {
                info.set_nat_destination_port(nat_flow->key().src_port);
            }
            info.set_nat_protocol(nat_flow->key().protocol);
            info.set_nat_vrf(data_.dest_vrf);
            info.set_reverse_index(nat_flow->flow_handle());
            info.set_nat_mirror_vrf(nat_flow->data().mirror_vrf);
        }
    }

    if (data_.match_p.action_info.action & (1 << TrafficAction::MIRROR)) {
        info.set_mirror(true);
        std::vector<MirrorActionSpec>::iterator it;
        std::vector<MirrorInfo> mirror_l;
        for (it = data_.match_p.action_info.mirror_l.begin();
             it != data_.match_p.action_info.mirror_l.end();
             ++it) {
            MirrorInfo mirror_info;
            mirror_info.set_mirror_destination((*it).ip.to_string());
            mirror_info.set_mirror_port((*it).port);
            mirror_info.set_mirror_vrf((*it).vrf_name);
            mirror_info.set_analyzer((*it).analyzer_name);
            mirror_l.push_back(mirror_info);
        }
        info.set_mirror_l(mirror_l);
    }
    info.set_mirror_vrf(data_.mirror_vrf);
    info.set_implicit_deny(ImplicitDenyFlow());
    info.set_short_flow(is_flags_set(FlowEntry::ShortFlow));
    if (is_flags_set(FlowEntry::EcmpFlow) && 
            data_.component_nh_idx != CompositeNH::kInvalidComponentNHIdx) {
        info.set_ecmp_index(data_.component_nh_idx);
    }
    if (is_flags_set(FlowEntry::Trap)) {
        info.set_trap(true);
    }
}

bool FlowEntry::FlowSrcMatch(const RouteFlowKey &rkey) const {
    uint32_t prefix = rkey.GetPrefix(key_.src.ipv4, data_.source_plen);
    if (data_.flow_source_vrf == rkey.vrf &&
        prefix == rkey.ip.ipv4 &&
        data_.source_plen == rkey.plen) {
        return true;
    }
    return false;
}

bool FlowEntry::FlowDestMatch(const RouteFlowKey &rkey) const {
    uint32_t prefix = rkey.GetPrefix(key_.dst.ipv4, data_.dest_plen);
    if (data_.flow_dest_vrf == rkey.vrf &&
        prefix == rkey.ip.ipv4 &&
        data_.dest_plen == rkey.plen) {
        return true;
    }
    return false;
}

void FlowEntry::UpdateReflexiveAction() {
    data_.match_p.sg_action = (1 << TrafficAction::PASS);
    data_.match_p.out_sg_action = (1 << TrafficAction::PASS);
    data_.match_p.reverse_sg_action = (1 << TrafficAction::PASS);;
    data_.match_p.reverse_out_sg_action = (1 << TrafficAction::PASS);
    data_.match_p.sg_action_summary = (1 << TrafficAction::PASS);

    FlowEntry *fwd_flow = reverse_flow_entry();
    if (fwd_flow) {
        data_.match_p.sg_action_summary =
            fwd_flow->data().match_p.sg_action_summary;
    }
    // If forward flow is DROP, set action for reverse flow to
    // TRAP. If packet hits reverse flow, we will re-establish
    // the flows
    if (ShouldDrop(data_.match_p.sg_action_summary)) {
        data_.match_p.sg_action &= ~(TrafficAction::DROP_FLAGS);
        data_.match_p.sg_action |= (1 << TrafficAction::TRAP);
     }
}

void FlowEntry::SetAclFlowSandeshData(const AclDBEntry *acl,
        FlowSandeshData &fe_sandesh_data) const {
    fe_sandesh_data.set_vrf(integerToString(key_.vrf));
    fe_sandesh_data.set_src(Ip4Address(key_.src.ipv4).to_string());
    fe_sandesh_data.set_dst(Ip4Address(key_.dst.ipv4).to_string());
    fe_sandesh_data.set_src_port(key_.src_port);
    fe_sandesh_data.set_dst_port(key_.dst_port);
    fe_sandesh_data.set_protocol(key_.protocol);
    fe_sandesh_data.set_ingress(is_flags_set(FlowEntry::IngressDir));
    std::vector<ActionStr> action_str_l;
    SetActionStr(data_.match_p.action_info, action_str_l);
    fe_sandesh_data.set_action_l(action_str_l);

    std::vector<AclAction> acl_action_l;
    SetAclAction(acl_action_l);
    fe_sandesh_data.set_acl_action_l(acl_action_l);

    fe_sandesh_data.set_flow_uuid(UuidToString(flow_uuid_));
    fe_sandesh_data.set_flow_handle(integerToString(flow_handle_));
    fe_sandesh_data.set_source_vn(data_.source_vn);
    fe_sandesh_data.set_dest_vn(data_.dest_vn);
    std::vector<uint32_t> v;
    SecurityGroupList::const_iterator it;
    for (it = data_.source_sg_id_l.begin(); 
            it != data_.source_sg_id_l.end(); it++) {
        v.push_back(*it);
    }
    fe_sandesh_data.set_source_sg_id_l(v);
    v.clear();
    for (it = data_.dest_sg_id_l.begin(); it != data_.dest_sg_id_l.end(); it++) {
        v.push_back(*it);
    }
    fe_sandesh_data.set_dest_sg_id_l(v);
    fe_sandesh_data.set_bytes(integerToString(stats_.bytes));
    fe_sandesh_data.set_packets(integerToString(stats_.packets));
    fe_sandesh_data.set_setup_time(
            integerToString(UTCUsecToPTime(stats_.setup_time)));
    fe_sandesh_data.set_setup_time_utc(stats_.setup_time);
    if (stats_.teardown_time) {
        fe_sandesh_data.set_teardown_time(
                integerToString(UTCUsecToPTime(stats_.teardown_time)));
    } else {
        fe_sandesh_data.set_teardown_time("");
    }
    fe_sandesh_data.set_current_time(integerToString(
                UTCUsecToPTime(UTCTimestampUsec())));

    SetAclListAceId(acl, data_.match_p.m_acl_l, fe_sandesh_data.ace_l);
    SetAclListAceId(acl, data_.match_p.m_sg_acl_l, fe_sandesh_data.ace_l);
    SetAclListAceId(acl, data_.match_p.m_mirror_acl_l, fe_sandesh_data.ace_l);
    SetAclListAceId(acl, data_.match_p.m_out_acl_l, fe_sandesh_data.ace_l);
    SetAclListAceId(acl, data_.match_p.m_reverse_sg_acl_l, fe_sandesh_data.ace_l);
    SetAclListAceId(acl, data_.match_p.m_reverse_out_sg_acl_l, fe_sandesh_data.ace_l);
    SetAclListAceId(acl, data_.match_p.m_out_sg_acl_l, fe_sandesh_data.ace_l);
    SetAclListAceId(acl, data_.match_p.m_out_mirror_acl_l, fe_sandesh_data.ace_l);

    fe_sandesh_data.set_reverse_flow(is_flags_set(FlowEntry::ReverseFlow) ?
                                     "yes" : "no");
    fe_sandesh_data.set_nat(is_flags_set(FlowEntry::NatFlow) ? "yes" : "no");
    fe_sandesh_data.set_implicit_deny(ImplicitDenyFlow() ? "yes" : "no");
    fe_sandesh_data.set_short_flow(is_flags_set(FlowEntry::ShortFlow) ? 
                                   "yes" : "no");

}

bool FlowEntry::SetRpfNH(const Inet4UnicastRouteEntry *rt) {
    const NextHop *nh = rt->GetActiveNextHop();
    if (nh->GetType() == NextHop::COMPOSITE &&
        !is_flags_set(FlowEntry::LocalFlow) &&
        is_flags_set(FlowEntry::IngressDir)) {
            //Logic for RPF check for ecmp
            //  Get reverse flow, and its corresponding ecmp index
            //  Check if source matches composite nh in reverse flow ecmp index,
            //  if not DP would trap packet for ECMP resolve.
            //  If there is only one instance of ECMP in compute node, then 
            //  RPF NH would only point to local interface NH.
            //  If there are multiple instances of ECMP in local server
            //  then RPF NH would point to local composite NH(containing 
            //  local members only)
        const CompositeNH *comp_nh = static_cast<const CompositeNH *>(nh);
        nh = comp_nh->GetLocalNextHop();
    }

    const NhState *nh_state = NULL;
    if (nh) {
        nh_state = static_cast<const NhState *>(
                nh->GetState(Agent::GetInstance()->GetNextHopTable(),
                    Agent::GetInstance()->pkt()->flow_table()->
                    nh_listener_id()));
        // With encap change nexthop can change for route. Route change
        // can come before nh change and it may skip using NH if nhstate is
        // not set. This may result in inconsistent flow-NH map.
        // So add new state if active nexthop in route does not have it.
        if (!nh->IsDeleted() && !nh_state) {
            DBEntryBase::KeyPtr key = nh->GetDBRequestKey();
            NextHopKey *nh_key = static_cast<NextHopKey *>(key.get());
            NextHop * new_nh = static_cast<NextHop *>(Agent::GetInstance()->
                               GetNextHopTable()->FindActiveEntry(nh_key));
            DBTablePartBase *part = Agent::GetInstance()->GetNextHopTable()->
                GetTablePartition(new_nh);
            NhState *new_nh_state = new NhState(new_nh);
            new_nh->SetState(part->parent(), Agent::GetInstance()->pkt()->
                         flow_table()->nh_listener_id(), new_nh_state);
            nh_state = new_nh_state;
        }
    }

    if (data_.nh_state_ != nh_state) {
        data_.nh_state_ = nh_state;
        return true;
    }
    return false;
}

bool FlowEntry::InitFlowCmn(const PktFlowInfo *info, const PktControlInfo *ctrl,
                            const PktControlInfo *rev_ctrl) {
    if (stats_.last_modified_time) {
        if (is_flags_set(FlowEntry::NatFlow) != info->nat_done) {
            MakeShortFlow();
            return false;
        }
        stats_.last_modified_time = UTCTimestampUsec();
    } else {
        /* For Flow Entry Create take last modified time same as setup time */
        stats_.last_modified_time = stats_.setup_time;
    }

    if (info->linklocal_flow) {
        set_flags(FlowEntry::LinkLocalFlow);
    } else {
        reset_flags(FlowEntry::LinkLocalFlow);
    }
    if (info->nat_done) {
        set_flags(FlowEntry::NatFlow);
    } else {
        reset_flags(FlowEntry::NatFlow);
    }
    if (info->short_flow) {
        set_flags(FlowEntry::ShortFlow);
    } else {
        reset_flags(FlowEntry::ShortFlow);
    }
    if (info->local_flow) {
        set_flags(FlowEntry::LocalFlow);
    } else {
        reset_flags(FlowEntry::LocalFlow);
    }

    if (info->tcp_ack) {
        set_flags(FlowEntry::TcpAckFlow);
    } else {
        reset_flags(FlowEntry::TcpAckFlow);
    }

    data_.intf_entry = ctrl->intf_ ? ctrl->intf_ : rev_ctrl->intf_;
    data_.vn_entry = ctrl->vn_ ? ctrl->vn_ : rev_ctrl->vn_;
    data_.in_vm_entry = ctrl->vm_ ? ctrl->vm_ : NULL;
    data_.out_vm_entry = rev_ctrl->vm_ ? rev_ctrl->vm_ : NULL;

    return true;
}

void FlowEntry::InitFwdFlow(const PktFlowInfo *info, const PktInfo *pkt,
                            const PktControlInfo *ctrl,
                            const PktControlInfo *rev_ctrl) {
    if (flow_handle_ != pkt->GetAgentHdr().cmd_param) {
        if (flow_handle_ != FlowEntry::kInvalidFlowHandle) {
            LOG(DEBUG, "Flow index changed from " << flow_handle_ 
                << " to " << pkt->GetAgentHdr().cmd_param);
        }
        flow_handle_ = pkt->GetAgentHdr().cmd_param;
    }

    if (InitFlowCmn(info, ctrl, rev_ctrl) == false) {
        return;
    }
    if (info->linklocal_bind_local_port) {
        linklocal_src_port_ = info->nat_sport;
        linklocal_src_port_fd_ = info->linklocal_src_port_fd;
        set_flags(FlowEntry::LinkLocalBindLocalSrcPort);
    } else {
        reset_flags(FlowEntry::LinkLocalBindLocalSrcPort);
    }
    stats_.intf_in = pkt->GetAgentHdr().ifindex;

    if (info->ingress) {
        set_flags(FlowEntry::IngressDir);
    } else {
        reset_flags(FlowEntry::IngressDir);
    }
    if (ctrl->rt_ != NULL) {
        SetRpfNH(ctrl->rt_);
    }
    data_.source_vn = *(info->source_vn);
    data_.dest_vn = *(info->dest_vn);
    data_.source_sg_id_l = *(info->source_sg_id_l);
    data_.dest_sg_id_l = *(info->dest_sg_id_l);
    data_.flow_source_vrf = info->flow_source_vrf;
    data_.flow_dest_vrf = info->flow_dest_vrf;
    data_.dest_vrf = info->dest_vrf;

    if (info->ecmp) {
        set_flags(FlowEntry::EcmpFlow);
    } else {
        reset_flags(FlowEntry::EcmpFlow);
    }
    data_.component_nh_idx = info->out_component_nh_idx;
    reset_flags(FlowEntry::Trap);
    data_.source_plen = info->source_plen;
    data_.dest_plen = info->dest_plen;
    if (ctrl->rt_ && ctrl->rt_->is_multicast()) {
        set_flags(FlowEntry::Multicast);
    }
    if (rev_ctrl->rt_ && rev_ctrl->rt_->is_multicast()) {
        set_flags(FlowEntry::Multicast);
    }
}

void FlowEntry::InitRevFlow(const PktFlowInfo *info,
                            const PktControlInfo *ctrl,
                            const PktControlInfo *rev_ctrl) {
    if (InitFlowCmn(info, ctrl, rev_ctrl) == false) {
        return;
    }
    if (ctrl->intf_) {
        stats_.intf_in = ctrl->intf_->id();
    } else {
        stats_.intf_in = Interface::kInvalidIndex;
    }

    // Compute reverse flow fields
    reset_flags(FlowEntry::IngressDir);
    if (ctrl->intf_) {
        if (info->ComputeDirection(ctrl->intf_)) {
            set_flags(FlowEntry::IngressDir);
        } else {
            reset_flags(FlowEntry::IngressDir);
        }
    }
    if (ctrl->rt_ != NULL) {
        SetRpfNH(ctrl->rt_);
    }
    data_.source_vn = *(info->dest_vn);
    data_.dest_vn = *(info->source_vn);
    data_.source_sg_id_l = *(info->dest_sg_id_l);
    data_.dest_sg_id_l = *(info->source_sg_id_l);
    data_.flow_source_vrf = info->flow_dest_vrf;
    data_.flow_dest_vrf = info->flow_source_vrf;
    data_.dest_vrf = info->nat_dest_vrf;
    if (info->ecmp) {
        set_flags(FlowEntry::EcmpFlow);
    } else {
        reset_flags(FlowEntry::EcmpFlow);
    }
    data_.component_nh_idx = info->in_component_nh_idx;
    if (info->trap_rev_flow) {
        set_flags(FlowEntry::Trap);
    } else {
        reset_flags(FlowEntry::Trap);
    }
    data_.source_plen = info->dest_plen;
    data_.dest_plen = info->source_plen;
}

void FlowEntry::InitAuditFlow(uint32_t flow_idx) {
    flow_handle_ = flow_idx;
    set_flags(FlowEntry::ShortFlow);
    data_.source_vn = *FlowHandler::UnknownVn();
    data_.dest_vn = *FlowHandler::UnknownVn();
    SecurityGroupList empty_sg_id_l;
    data_.source_sg_id_l = empty_sg_id_l;
    data_.dest_sg_id_l = empty_sg_id_l;
}

FlowEntry *FlowTable::Allocate(const FlowKey &key) {
    FlowEntry *flow = new FlowEntry(key);
    std::pair<FlowEntryMap::iterator, bool> ret;
    ret = flow_entry_map_.insert(std::pair<FlowKey, FlowEntry*>(key, flow));
    if (ret.second == false) {
        delete flow;
        flow = ret.first->second;
        flow->set_deleted(false);
        DeleteFlowInfo(flow);
    } else {
        flow->stats_.setup_time = UTCTimestampUsec();
        agent_->stats()->incr_flow_created();
    }

    return flow;
}

FlowEntry *FlowTable::Find(const FlowKey &key) {
    FlowEntryMap::iterator it;

    it = flow_entry_map_.find(key);
    if (it != flow_entry_map_.end()) {
        return it->second;
    } else {
        return NULL;
    }
}

void FlowTable::DeleteInternal(FlowEntryMap::iterator &it)
{
    FlowInfo flow_info;
    FlowEntry *fe = it->second;
    if (fe->deleted()) {
        /* Already deleted return from here. */
        return;
    }
    fe->set_deleted(true);
    fe->FillFlowInfo(flow_info);
    FLOW_TRACE(Trace, "Delete", flow_info);
    FlowTableKSyncObject *ksync_obj = 
        agent_->ksync()->flowtable_ksync_obj();

    FlowStatsCollector *fec = agent_->uve()->flow_stats_collector();
    uint64_t diff_bytes, diff_packets;
    fec->UpdateFlowStats(fe, diff_bytes, diff_packets);

    fe->stats_.teardown_time = UTCTimestampUsec();
    fec->FlowExport(fe, diff_bytes, diff_packets);
    /* Reset stats and teardown_time after these information is exported during
     * flow delete so that if the flow entry is reused they point to right 
     * values */
    fe->ResetStats();
    fe->stats_.teardown_time = 0;

    // Unlink the reverse flow, if one exists
    FlowEntry *rflow = fe->reverse_flow_entry();
    if (rflow) {
        rflow->set_reverse_flow_entry(NULL);
    }
    fe->set_reverse_flow_entry(NULL);

    DeleteFlowInfo(fe);

    FlowTableKSyncEntry *ksync_entry = fe->ksync_entry_;
    KSyncEntry::KSyncEntryPtr ksync_ptr = ksync_entry;
    if (ksync_entry) {
        ksync_obj->Delete(ksync_entry);
        fe->ksync_entry_ = NULL;
    } else {
        FLOW_TRACE(Err, fe->flow_handle(), "Entry not found in ksync");
        if (fe->reverse_flow_entry() != NULL) {
            fe->set_reverse_flow_entry(NULL);
        }
    }

    agent_->stats()->incr_flow_aged();
}

bool FlowTable::Delete(FlowEntryMap::iterator &it, bool rev_flow)
{
    FlowEntry *fe;
    FlowEntryMap::iterator rev_it;

    fe = it->second;
    FlowEntry *reverse_flow = NULL;
    if (fe->is_flags_set(FlowEntry::NatFlow) || rev_flow) {
        reverse_flow = fe->reverse_flow_entry();
    }
    DeleteInternal(it);

    if (!reverse_flow) {
        return true;
    }
    /* If reverse-flow is valid and the present iterator is pointing to it,
     * use that iterator to delete reverse flow
     */
    if (reverse_flow == it->second) {
        DeleteInternal(it);
        return true;
    }

    rev_it = flow_entry_map_.find(reverse_flow->key());
    if (rev_it != flow_entry_map_.end()) {
        DeleteInternal(rev_it);
        return true;
    }
    return false;
}

bool FlowTable::Delete(const FlowKey &key, bool del_reverse_flow)
{
    FlowEntryMap::iterator it;
    FlowEntry *fe;

    it = flow_entry_map_.find(key);
    if (it == flow_entry_map_.end()) {
        return false;
    }
    fe = it->second;

    FlowEntry *reverse_flow = NULL;
    if (del_reverse_flow) {
        reverse_flow = fe->reverse_flow_entry();
    }

    /* Delete the forward flow */
    DeleteInternal(it);

    if (!reverse_flow) {
        return true;
    }

    it = flow_entry_map_.find(reverse_flow->key());
    if (it != flow_entry_map_.end()) {
        DeleteInternal(it);
        return true;
    }
    return false;
}

void FlowTable::DeleteAll()
{
    FlowEntryMap::iterator it;

    it = flow_entry_map_.begin();
    while (it != flow_entry_map_.end()) {
        FlowEntry *entry = it->second;
        ++it;
        if (it != flow_entry_map_.end() &&
            it->second == entry->reverse_flow_entry()) {
            ++it;
        }
        Delete(entry->key(), true);
    }
}

void FlowTable::DeleteAclFlows(const AclDBEntry *acl)
{
    AclFlowTree::iterator it;
    it = acl_flow_tree_.find(acl);
    if (it == acl_flow_tree_.end()) {
        return;
    }
    // Get the ACL flow tree
    AclFlowInfo *af_info = it->second;
    FlowEntryTree fe_tree = af_info->fet;
    FlowEntryTree::iterator fe_tree_it;
    fe_tree_it  = fe_tree.begin();
    while(fe_tree_it != fe_tree.end()) {
        const FlowKey &fekey = (*fe_tree_it)->key();
        ++fe_tree_it;
        Delete(fekey, true);
    }
}

SandeshTraceBufferPtr FlowTraceBuf(SandeshTraceBufferCreate("Flow", 5000));

void FlowTable::Init() {

    FlowEntry::alloc_count_ = 0;

    acl_listener_id_ = agent_->GetAclTable()->Register
        (boost::bind(&FlowTable::AclNotify, this, _1, _2));

    intf_listener_id_ = agent_->GetInterfaceTable()->Register
        (boost::bind(&FlowTable::IntfNotify, this, _1, _2));

    vn_listener_id_ = agent_->GetVnTable()->Register
        (boost::bind(&FlowTable::VnNotify, this, _1, _2));

    vrf_listener_id_ = agent_->GetVrfTable()->Register
            (boost::bind(&FlowTable::VrfNotify, this, _1, _2));

    nh_listener_ = new NhListener();
    return;
}

void FlowTable::Shutdown() {
}

void FlowTable::IntfNotify(DBTablePartBase *part, DBEntryBase *e) {
    // Add/Delete SG: Later
    // Change VN:
    // Resync all intf flows with new VN network policies + SG
    Interface *intf = static_cast<Interface *>(e);
    if (intf->type() != Interface::VM_INTERFACE) {
        return;
    }

    VmInterface *vm_port = static_cast<VmInterface *>(intf);
    const VnEntry *new_vn = vm_port->vn();

    DBState *s = e->GetState(part->parent(), intf_listener_id_);
    VmIntfFlowHandlerState *state = static_cast<VmIntfFlowHandlerState *>(s);

    if (intf->IsDeleted() || new_vn == NULL) {
        DeleteVmIntfFlows(intf);
        if (state) {
            e->ClearState(part->parent(), intf_listener_id_);
            delete state;
        }
        return;
    }

    const VmInterface::SecurityGroupEntryList &new_sg_l = vm_port->sg_list();
    bool changed = false;

    if (state == NULL) {
        state = new VmIntfFlowHandlerState(NULL);
        e->SetState(part->parent(), intf_listener_id_, state);
        // Force change for first time
        state->policy_ = !vm_port->policy_enabled();
        state->sg_l_ = new_sg_l;
        state->vn_ = new_vn;
        changed = true;
    } else {
        if (state->vn_.get() != new_vn) {
            changed = true;
            state->vn_ = new_vn;
        }
        if (state->policy_ != vm_port->policy_enabled()) {
            changed = true;
            state->policy_ = vm_port->policy_enabled();
        }
        if (state->sg_l_.list_ != new_sg_l.list_) {
            changed = true;
            state->sg_l_ = new_sg_l;
        }
    }

    if (changed) {
        ResyncVmPortFlows(vm_port);
    }
}

void FlowTable::VnNotify(DBTablePartBase *part, DBEntryBase *e) 
{
    // Add/Delete Acl:
    // Resync all Vn flows with new VN network policies
    VnEntry *vn = static_cast<VnEntry *>(e);
    DBState *s = e->GetState(part->parent(), vn_listener_id_);
    VnFlowHandlerState *state = static_cast<VnFlowHandlerState *>(s);
    AclDBEntryConstRef acl = NULL;
    AclDBEntryConstRef macl = NULL;
    AclDBEntryConstRef mcacl = NULL;

    if (vn->IsDeleted()) {
        DeleteVnFlows(vn);
        if (state) {
            e->ClearState(part->parent(), vn_listener_id_);
            delete state;
        }
        return;
    }

    if (state != NULL) { 
        acl = state->acl_;
        macl = state->macl_;
        mcacl = state->mcacl_;
    }

    const AclDBEntry *new_acl = vn->GetAcl();
    const AclDBEntry *new_macl = vn->GetMirrorAcl();
    const AclDBEntry *new_mcacl = vn->GetMirrorCfgAcl();
    
    if (state == NULL) {
        state = new VnFlowHandlerState(new_acl, new_macl, new_mcacl);
        e->SetState(part->parent(), vn_listener_id_, state);
    }

    if (acl != new_acl || macl != new_macl || mcacl !=new_mcacl) {
        state->acl_ = new_acl;
        state->macl_ = new_macl;
        state->mcacl_ = new_mcacl;
        ResyncVnFlows(vn);
    }
}

void FlowTable::AclNotify(DBTablePartBase *part, DBEntryBase *e) 
{
    // Delete ACL: (could be ignored), VN gets anyway notification of delete ACL.
    // Modify ACL:
    // Get VN 
    // Resync with VN network policies
    AclDBEntry *acl = static_cast<AclDBEntry *>(e);
    if (e->IsDeleted()) {
        // VN entry must have got updated and VnNotify will take care of the chnages.
        // no need to do any here.
        DeleteAclFlows(acl);
    } else {
        ResyncAclFlows(acl);
    }
}

Inet4RouteUpdate::Inet4RouteUpdate(Inet4UnicastAgentRouteTable *rt_table):
    rt_table_(rt_table), marked_delete_(false), 
    table_delete_ref_(this, rt_table->deleter()) {
}

Inet4RouteUpdate::~Inet4RouteUpdate() {
    if (rt_table_) {
        rt_table_->Unregister(id_);
    }
    table_delete_ref_.Reset(NULL);
}

void Inet4RouteUpdate::ManagedDelete() {
    marked_delete_ = true;
}

bool Inet4RouteUpdate::DeleteState(DBTablePartBase *partition,
                                   DBEntryBase *entry) {
    State *state = static_cast<State *>
                          (entry->GetState(partition->parent(), id_));
    if (state) {
        entry->ClearState(partition->parent(), id_);
        delete state;
    }
    return true;
}

void Inet4RouteUpdate::WalkDone(DBTableBase *partition,
                                Inet4RouteUpdate *rt_update) {
    delete rt_update;
}

void Inet4RouteUpdate::Unregister() {
    DBTableWalker *walker = Agent::GetInstance()->GetDB()->GetWalker();
    walker->WalkTable(rt_table_, NULL,
                      boost::bind(&Inet4RouteUpdate::DeleteState, this, _1, _2),
                      boost::bind(&Inet4RouteUpdate::WalkDone, _1, this));
}

void NhListener::Notify(DBTablePartBase *part, DBEntryBase *e) {
    NextHop *nh = static_cast<NextHop *>(e);
    NhState *state = 
        static_cast<NhState *>(e->GetState(part->parent(), id_));

    if (nh->IsDeleted()) {
        if (state && state->refcount() == 0) {
            e->ClearState(part->parent(), id_);
            delete state;
        }
        return;
    }

    if (!state) {
        state = new NhState(nh);
    }
    nh->SetState(part->parent(), id_, state);
    return; 
}

void Inet4RouteUpdate::UnicastNotify(DBTablePartBase *partition, DBEntryBase *e)
{
    Inet4UnicastRouteEntry *route = static_cast<Inet4UnicastRouteEntry *>(e);
    State *state = static_cast<State *>(e->GetState(partition->parent(), id_));

    if (route->is_multicast()) {
        return;
    }
    
    SecurityGroupList new_sg_l;
    if (route->GetActivePath()) {
        new_sg_l = route->GetActivePath()->sg_list();
    }
    FLOW_TRACE(RouteUpdate, 
               route->vrf()->GetName(), 
               route->addr().to_string(), 
               route->plen(), 
               (route->GetActivePath()) ? route->dest_vn_name() : "",
               route->IsDeleted(),
               marked_delete_,
               new_sg_l.size(),
               new_sg_l);

    // Handle delete cases
    if (marked_delete_ || route->IsDeleted()) {
        RouteFlowKey rkey(route->vrf()->vrf_id(),
                          route->addr().to_ulong(), route->plen());
        Agent::GetInstance()->pkt()->flow_table()->DeleteRouteFlows(rkey);
        if (state) {
            route->ClearState(partition->parent(), id_);
            delete state;
        }
        return;
    }

    if (state == NULL) {
        state  = new State();
        route->SetState(partition->parent(), id_, state);
    }

    RouteFlowKey skey(route->vrf()->vrf_id(), 
                      route->addr().to_ulong(), route->plen());
    sort (new_sg_l.begin(), new_sg_l.end());
    if (state->sg_l_ != new_sg_l) {
        state->sg_l_ = new_sg_l;
        Agent::GetInstance()->pkt()->flow_table()->ResyncRouteFlows(skey, new_sg_l);
    }

    //Trigger RPF NH sync, if active nexthop changes
    const NextHop *active_nh = route->GetActiveNextHop();
    const NextHop *local_nh = NULL;
    if (active_nh->GetType() == NextHop::COMPOSITE) {
        //If destination is ecmp, all remote flow would
        //have RPF NH set to that local component NH
        const CompositeNH *comp_nh = 
            static_cast<const CompositeNH *>(active_nh);
        local_nh = comp_nh->GetLocalCompositeNH();
    }

    if ((state->active_nh_ != active_nh) || (state->local_nh_ != local_nh)) {
        Agent::GetInstance()->pkt()->flow_table()->ResyncRpfNH(skey, route);
        state->active_nh_ = active_nh;
        state->local_nh_ = local_nh;
    }
}

Inet4RouteUpdate *Inet4RouteUpdate::UnicastInit(
                              Inet4UnicastAgentRouteTable *table)
{
    Inet4RouteUpdate *rt_update = new Inet4RouteUpdate(table);
    rt_update->id_ = table->Register(
        boost::bind(&Inet4RouteUpdate::UnicastNotify, rt_update, _1, _2));
    return rt_update;
}


void FlowTable::VrfNotify(DBTablePartBase *part, DBEntryBase *e)
{   
    VrfEntry *vrf = static_cast<VrfEntry *>(e);
    DBState *s = e->GetState(part->parent(), vrf_listener_id_);
    VrfFlowHandlerState *state = static_cast<VrfFlowHandlerState *>(s);
    if (vrf->IsDeleted()) {
        if (state == NULL) {
            return;
        }
        state->inet4_unicast_update_->Unregister();
        e->ClearState(part->parent(), vrf_listener_id_);
        delete state;
        return;
    }
    if (state == NULL) {
        state = new VrfFlowHandlerState();
        state->inet4_unicast_update_ = 
            Inet4RouteUpdate::UnicastInit(
            static_cast<Inet4UnicastAgentRouteTable *>(vrf->
            GetInet4UnicastRouteTable()));
        vrf->SetState(part->parent(), vrf_listener_id_, state);
    }
    return;
}

void FlowTable::ResyncVnFlows(const VnEntry *vn) {
    VnFlowTree::iterator vn_it;
    vn_it = vn_flow_tree_.find(vn);
    if (vn_it == vn_flow_tree_.end()) {
        return;
    }

    FlowEntryTree fet = vn_it->second->fet;
    FlowEntryTree::iterator fet_it, it;
    it = fet.begin();
    while (it != fet.end()) {
        fet_it = it++;
        FlowEntry *fe = (*fet_it).get();
        DeleteFlowInfo(fe);
        fe->GetPolicyInfo(vn);
        ResyncAFlow(fe);
        AddFlowInfo(fe);
        FlowInfo flow_info;
        fe->FillFlowInfo(flow_info);
        FLOW_TRACE(Trace, "Evaluate Vn Flows", flow_info);
    }
}

void FlowTable::ResyncAclFlows(const AclDBEntry *acl)
{
    AclFlowTree::iterator acl_it;
    acl_it = acl_flow_tree_.find(acl);
    if (acl_it == acl_flow_tree_.end()) {
        return;
    }

    FlowEntryTree fet = acl_it->second->fet;
    FlowEntryTree::iterator fet_it, it;
    it = fet.begin();
    while (it != fet.end()) {
        fet_it = it++;
        FlowEntry *fe = (*fet_it).get();
        DeleteFlowInfo(fe);
        fe->GetPolicyInfo();
        ResyncAFlow(fe);
        AddFlowInfo(fe);
        FlowInfo flow_info;
        fe->FillFlowInfo(flow_info);
        FLOW_TRACE(Trace, "Evaluate Acl Flows", flow_info);
    }
}

void FlowTable::ResyncRpfNH(const RouteFlowKey &key, 
                            const Inet4UnicastRouteEntry *rt) {
    RouteFlowTree::iterator rf_it;
    rf_it = route_flow_tree_.find(key);
    if (rf_it == route_flow_tree_.end()) {
        return;
    }
    FlowEntryTree fet = rf_it->second->fet;
    FlowEntryTree::iterator fet_it, it;
    it = fet.begin();
    while (it != fet.end()) {
        fet_it = it++;
        FlowEntry *flow = (*fet_it).get();
        if (flow->FlowSrcMatch(key) == false) {
            continue;
        }

        if (flow->SetRpfNH(rt) == true) {
            flow->UpdateKSync();
            FlowInfo flow_info;
            flow->FillFlowInfo(flow_info);
            FLOW_TRACE(Trace, "Resync RPF NH", flow_info);
        }
    }
}

void FlowTable::ResyncRouteFlows(RouteFlowKey &key, SecurityGroupList &sg_l)
{
    RouteFlowTree::iterator rf_it;
    rf_it = route_flow_tree_.find(key);
    if (rf_it == route_flow_tree_.end()) {
        return;
    }
    FlowEntryTree fet = rf_it->second->fet;
    FlowEntryTree::iterator fet_it, it;
    it = fet.begin();
    while (it != fet.end()) {
        fet_it = it++;
        FlowEntry *fe = (*fet_it).get();
        DeleteFlowInfo(fe);
        fe->GetPolicyInfo();
        if (fe->FlowSrcMatch(key)) {
            fe->set_source_sg_id_l(sg_l);
        } else if (fe->FlowDestMatch(key)) {
            fe->set_dest_sg_id_l(sg_l);
        } else {
            FLOW_TRACE(Err, fe->flow_handle(), 
                       "Not found route key, vrf:"
                       + integerToString(key.vrf) 
                       + " ip:"
                       + Ip4Address(key.ip.ipv4).to_string());
        }
        ResyncAFlow(fe);
        AddFlowInfo(fe);
        FlowInfo flow_info;
        fe->FillFlowInfo(flow_info);
        FLOW_TRACE(Trace, "Evaluate Route Flows", flow_info);
    }
}

void FlowTable::ResyncVmPortFlows(const VmInterface *intf) {
    IntfFlowTree::iterator intf_it;
    intf_it = intf_flow_tree_.find(intf);
    if (intf_it == intf_flow_tree_.end()) {
        return;
    }

    FlowEntryTree fet = intf_it->second->fet;
    FlowEntryTree::iterator fet_it, it;
    it = fet.begin();
    while (it != fet.end()) {
        fet_it = it++;
        FlowEntry *fe = (*fet_it).get();
        // Local flow needs to evaluate fwd flow then reverse flow
        if (fe->is_flags_set(FlowEntry::LocalFlow) && 
            fe->is_flags_set(FlowEntry::ReverseFlow)) {
            FlowEntry *fwd_flow = fe->reverse_flow_entry();
            if (fwd_flow) {
                DeleteFlowInfo(fwd_flow);
                fwd_flow->GetPolicyInfo();
                ResyncAFlow(fwd_flow);
                AddFlowInfo(fwd_flow);
                FlowInfo flow_info;
                fwd_flow->FillFlowInfo(flow_info);
                FLOW_TRACE(Trace, "Evaluate VmPort Flows", flow_info);
            }
        }
        DeleteFlowInfo(fe);
        fe->GetPolicyInfo(intf->vn());
        ResyncAFlow(fe);
        AddFlowInfo(fe);
        FlowInfo flow_info;
        fe->FillFlowInfo(flow_info);
        FLOW_TRACE(Trace, "Evaluate VmPort Flows", flow_info);
    }
}


void FlowTable::DeleteRouteFlows(const RouteFlowKey &key)
{
    RouteFlowTree::iterator rf_it;
    rf_it = route_flow_tree_.find(key);
    if (rf_it == route_flow_tree_.end()) {
        return;
    }
    FLOW_TRACE(ModuleInfo, "Delete Route flows");
    FlowEntryTree fet = rf_it->second->fet;
    FlowEntryTree::iterator fet_it, it;
    it = fet.begin();
    while (it != fet.end()) {
        fet_it = it++;
        FlowEntry *fe = (*fet_it).get();
        Delete(fe->key(), true);
    }
}

void FlowTable::DeleteFlowInfo(FlowEntry *fe) 
{
    agent_->uve()->DeleteFlow(fe);
    // Remove from AclFlowTree
    // Go to all matched ACL list and remove from all acls
    std::list<MatchAclParams>::const_iterator acl_it;
    for (acl_it = fe->match_p().m_acl_l.begin(); acl_it != fe->match_p().m_acl_l.end();
         ++acl_it) {
        DeleteAclFlowInfo((*acl_it).acl.get(), fe, (*acl_it).ace_id_list);
    }
    for (acl_it = fe->match_p().m_sg_acl_l.begin(); 
         acl_it != fe->match_p().m_sg_acl_l.end();
         ++acl_it) {
        DeleteAclFlowInfo((*acl_it).acl.get(), fe, (*acl_it).ace_id_list);
    }

    for (acl_it = fe->match_p().m_out_acl_l.begin();
         acl_it != fe->match_p().m_out_acl_l.end(); ++acl_it) {
        DeleteAclFlowInfo((*acl_it).acl.get(), fe, (*acl_it).ace_id_list);
    }
    for (acl_it = fe->match_p().m_out_sg_acl_l.begin(); 
         acl_it != fe->match_p().m_out_sg_acl_l.end();
         ++acl_it) {
        DeleteAclFlowInfo((*acl_it).acl.get(), fe, (*acl_it).ace_id_list);
    }

    for (acl_it = fe->match_p().m_reverse_sg_acl_l.begin();
         acl_it != fe->match_p().m_reverse_sg_acl_l.end(); ++acl_it) {
        DeleteAclFlowInfo((*acl_it).acl.get(), fe, (*acl_it).ace_id_list);
    }
    for (acl_it = fe->match_p().m_reverse_out_sg_acl_l.begin();
         acl_it != fe->match_p().m_reverse_out_sg_acl_l.end();
         ++acl_it) {
        DeleteAclFlowInfo((*acl_it).acl.get(), fe, (*acl_it).ace_id_list);
    }

    for (acl_it = fe->match_p().m_mirror_acl_l.begin(); 
         acl_it != fe->match_p().m_mirror_acl_l.end();
         ++acl_it) {
        DeleteAclFlowInfo((*acl_it).acl.get(), fe, (*acl_it).ace_id_list);
    }

    for (acl_it = fe->match_p().m_out_mirror_acl_l.begin(); 
         acl_it != fe->match_p().m_out_mirror_acl_l.end();
         ++acl_it) {
        DeleteAclFlowInfo((*acl_it).acl.get(), fe, (*acl_it).ace_id_list);
    }
    for (acl_it = fe->match_p().m_vrf_assign_acl_l.begin();
         acl_it != fe->match_p().m_vrf_assign_acl_l.end();
         ++acl_it) {
        DeleteAclFlowInfo((*acl_it).acl.get(), fe, (*acl_it).ace_id_list);
    }

    // Remove from IntfFlowTree
    DeleteIntfFlowInfo(fe);    
    // Remove from VnFlowTree
    DeleteVnFlowInfo(fe);
    // Remove from VmFlowTree
    DeleteVmFlowInfo(fe);
    // Remove from RouteFlowTree
    DeleteRouteFlowInfo(fe);
}

void FlowTable::DeleteVnFlowInfo(FlowEntry *fe)
{
    VnFlowTree::iterator vn_it;
    if (fe->vn_entry()) {
        vn_it = vn_flow_tree_.find(fe->vn_entry());
        if (vn_it != vn_flow_tree_.end()) {
            VnFlowInfo *vn_flow_info = vn_it->second;
            int count = vn_flow_info->fet.erase(fe);
            if (count > 0) {
                DecrVnFlowCounter(vn_flow_info, fe);
            }
            if (vn_flow_info->fet.empty()) {
                delete vn_flow_info;
                vn_flow_tree_.erase(vn_it);
            }
        }
    }
}

void FlowTable::DeleteAclFlowInfo(const AclDBEntry *acl, FlowEntry* flow,
        const AclEntryIDList &id_list)
{
    AclFlowTree::iterator acl_it;
    acl_it = acl_flow_tree_.find(acl);
    if (acl_it == acl_flow_tree_.end()) {
        return;
    }

    // Delete flow entry from the Flow entry list
    AclFlowInfo *af_info = acl_it->second;
    AclEntryIDList::const_iterator id_it;
    for (id_it = id_list.begin(); id_it != id_list.end(); ++id_it) {
        af_info->aceid_cnt_map[*id_it] -= 1;
    }
    af_info->fet.erase(flow);
    if (af_info->fet.empty()) {
        delete af_info;
        acl_flow_tree_.erase(acl_it);
    }
}

void FlowTable::DeleteIntfFlowInfo(FlowEntry *fe)
{
    IntfFlowTree::iterator intf_it;
    if (fe->intf_entry()) {
        intf_it = intf_flow_tree_.find(fe->intf_entry());
        if (intf_it != intf_flow_tree_.end()) {
            IntfFlowInfo *intf_flow_info = intf_it->second;
            intf_flow_info->fet.erase(fe);
            if (intf_flow_info->fet.empty()) {
                delete intf_flow_info;
                intf_flow_tree_.erase(intf_it);
            }
        }
    }
}

void FlowTable::DeleteVmFlowInfo(FlowEntry *fe) {
    if (fe->in_vm_entry()) {
        DeleteVmFlowInfo(fe, fe->in_vm_entry());
    }
    if (fe->out_vm_entry()) {
        DeleteVmFlowInfo(fe, fe->out_vm_entry());
    }
}

void FlowTable::DeleteVmFlowInfo(FlowEntry *fe, const VmEntry *vm) {
    VmFlowTree::iterator vm_it = vm_flow_tree_.find(vm);
    if (vm_it != vm_flow_tree_.end()) {
        VmFlowInfo *vm_flow_info = vm_it->second;
        if (vm_flow_info->fet.erase(fe)) {
            if (fe->linklocal_src_port()) {
                vm_flow_info->linklocal_flow_count--;
                linklocal_flow_count_--;
            }
            if (vm_flow_info->fet.empty()) {
                delete vm_flow_info;
                vm_flow_tree_.erase(vm_it);
            }
        }
    }
}

void FlowTable::DeleteRouteFlowInfo (FlowEntry *fe)
{
    RouteFlowTree::iterator rf_it;
    RouteFlowKey skey(fe->data().flow_source_vrf, fe->key().src.ipv4, 
                      fe->data().source_plen);
    rf_it = route_flow_tree_.find(skey);
    RouteFlowInfo *route_flow_info;
    if (rf_it != route_flow_tree_.end()) {
        route_flow_info = rf_it->second;
        route_flow_info->fet.erase(fe);
        if (route_flow_info->fet.empty()) {
            delete route_flow_info;
            route_flow_tree_.erase(rf_it);
        }
    }
   
    RouteFlowKey dkey(fe->data().flow_dest_vrf, fe->key().dst.ipv4,
                      fe->data().dest_plen);
    rf_it = route_flow_tree_.find(dkey);
    if (rf_it != route_flow_tree_.end()) {
        route_flow_info = rf_it->second;
        route_flow_info->fet.erase(fe);
        if (route_flow_info->fet.empty()) {
            delete route_flow_info;
            route_flow_tree_.erase(rf_it);
        }
    }
}

void FlowTable::AddFlowInfo(FlowEntry *fe)
{
    agent_->uve()->NewFlow(fe);
    // Add AclFlowTree
    AddAclFlowInfo(fe);
    // Add IntfFlowTree
    AddIntfFlowInfo(fe);
    // Add VnFlowTree
    AddVnFlowInfo(fe);
    // Add VmFlowTree
    AddVmFlowInfo(fe);
    // Add RouteFlowTree;
    AddRouteFlowInfo(fe);
}

void FlowTable::AddAclFlowInfo (FlowEntry *fe) 
{
    std::list<MatchAclParams>::const_iterator it;
    for (it = fe->match_p().m_acl_l.begin();
         it != fe->match_p().m_acl_l.end();
         ++it) {
        UpdateAclFlow(it->acl.get(), fe, it->ace_id_list);
    }
    for (it = fe->match_p().m_sg_acl_l.begin();
         it != fe->match_p().m_sg_acl_l.end();
         ++it) {
        UpdateAclFlow(it->acl.get(), fe, it->ace_id_list);
    }

    for (it = fe->match_p().m_out_acl_l.begin();
         it != fe->match_p().m_out_acl_l.end();
         ++it) {
        UpdateAclFlow(it->acl.get(), fe, it->ace_id_list);
    }
    for (it = fe->match_p().m_out_sg_acl_l.begin();
         it != fe->match_p().m_out_sg_acl_l.end();
         ++it) {
        UpdateAclFlow(it->acl.get(), fe, it->ace_id_list);
    }

    for (it = fe->match_p().m_reverse_sg_acl_l.begin();
         it != fe->match_p().m_reverse_sg_acl_l.end();
         ++it) {
        UpdateAclFlow(it->acl.get(), fe, it->ace_id_list);
    }
    for (it = fe->match_p().m_reverse_out_sg_acl_l.begin();
         it != fe->match_p().m_reverse_out_sg_acl_l.end();
         ++it) {
        UpdateAclFlow(it->acl.get(), fe, it->ace_id_list);
    }

    for (it = fe->match_p().m_mirror_acl_l.begin();
         it != fe->match_p().m_mirror_acl_l.end();
         ++it) {
        UpdateAclFlow(it->acl.get(), fe, it->ace_id_list);
    }
    for (it = fe->match_p().m_out_mirror_acl_l.begin();
         it != fe->match_p().m_out_mirror_acl_l.end();
         ++it) {
        UpdateAclFlow(it->acl.get(), fe, it->ace_id_list);
    }
    for (it = fe->match_p().m_vrf_assign_acl_l.begin();
            it != fe->match_p().m_vrf_assign_acl_l.end();
            ++it) {
        UpdateAclFlow(it->acl.get(), fe, it->ace_id_list);
    }
}

void FlowTable::UpdateAclFlow(const AclDBEntry *acl, FlowEntry* flow,
                              const AclEntryIDList &id_list)
{
    AclFlowTree::iterator it;
    pair<set<FlowEntryPtr>::iterator,bool> ret;

    it = acl_flow_tree_.find(acl);

    AclFlowInfo *af_info;
    if (it == acl_flow_tree_.end()) {
        af_info = new AclFlowInfo;
        af_info->acl_entry = acl;
        ret = af_info->fet.insert(flow);
        acl_flow_tree_.insert(AclFlowPair(acl, af_info));
    } else {
        af_info = it->second;
        /* flow can already exist. In that case it won't be inserted */
        ret = af_info->fet.insert(flow);
    }
    
    if (id_list.size()) {
        AclEntryIDList::const_iterator id_it;
        for (id_it = id_list.begin(); id_it != id_list.end(); ++id_it) {
            af_info->aceid_cnt_map[*id_it] += 1;
        }        
    } else {
        af_info->flow_miss++;
    }
}

void FlowTable::AddIntfFlowInfo(FlowEntry *fe)
{
    if (!fe->intf_entry()) {
        return;
    }
    IntfFlowTree::iterator it;
    it = intf_flow_tree_.find(fe->intf_entry());
    IntfFlowInfo *intf_flow_info;
    if (it == intf_flow_tree_.end()) {
        intf_flow_info = new IntfFlowInfo();
        intf_flow_info->intf_entry = fe->intf_entry();
        intf_flow_info->fet.insert(fe);
        intf_flow_tree_.insert(IntfFlowPair(fe->intf_entry(), intf_flow_info));
    } else {
        intf_flow_info = it->second;
        /* fe can already exist. In that case it won't be inserted */
        intf_flow_info->fet.insert(fe);
    }
}

void FlowTable::AddVmFlowInfo(FlowEntry *fe) {
    if (fe->in_vm_entry()) {
        AddVmFlowInfo(fe, fe->in_vm_entry());
    }
    if (fe->out_vm_entry()) {
        AddVmFlowInfo(fe, fe->out_vm_entry());
    }
}

void FlowTable::AddVmFlowInfo(FlowEntry *fe, const VmEntry *vm) {
    bool update = false;
    VmFlowTree::iterator it;
    it = vm_flow_tree_.find(vm);
    VmFlowInfo *vm_flow_info;
    if (it == vm_flow_tree_.end()) {
        vm_flow_info = new VmFlowInfo();
        vm_flow_info->vm_entry = vm;
        vm_flow_info->fet.insert(fe);
        vm_flow_tree_.insert(VmFlowPair(vm, vm_flow_info));
        update = true;
    } else {
        vm_flow_info = it->second;
        /* fe can already exist. In that case it won't be inserted */
        if (vm_flow_info->fet.insert(fe).second) {
            update = true;
        }
    }
    if (update) {
        if (fe->linklocal_src_port()) {
            vm_flow_info->linklocal_flow_count++;
            linklocal_flow_count_++;
        }
    }
}

void FlowTable::IncrVnFlowCounter(VnFlowInfo *vn_flow_info, 
                                  const FlowEntry *fe) {
    if (fe->is_flags_set(FlowEntry::LocalFlow)) {
        vn_flow_info->ingress_flow_count++;
        vn_flow_info->egress_flow_count++;
    } else {
        if (fe->is_flags_set(FlowEntry::IngressDir)) {
            vn_flow_info->ingress_flow_count++;
        } else {
            vn_flow_info->egress_flow_count++;
        }
    }
}

void FlowTable::DecrVnFlowCounter(VnFlowInfo *vn_flow_info, 
                                  const FlowEntry *fe) {
    if (fe->is_flags_set(FlowEntry::LocalFlow)) {
        vn_flow_info->ingress_flow_count--;
        vn_flow_info->egress_flow_count--;
    } else {
        if (fe->is_flags_set(FlowEntry::IngressDir)) {
            vn_flow_info->ingress_flow_count--;
        } else {
            vn_flow_info->egress_flow_count--;
        }
    }
}

void FlowTable::AddVnFlowInfo (FlowEntry *fe)
{
    if (!fe->vn_entry()) {
        return;
    }    
    VnFlowTree::iterator it;
    it = vn_flow_tree_.find(fe->vn_entry());
    VnFlowInfo *vn_flow_info;
    if (it == vn_flow_tree_.end()) {
        vn_flow_info = new VnFlowInfo();
        vn_flow_info->vn_entry = fe->vn_entry();
        vn_flow_info->fet.insert(fe);
        IncrVnFlowCounter(vn_flow_info, fe);
        vn_flow_tree_.insert(VnFlowPair(fe->vn_entry(), vn_flow_info));
    } else {
        vn_flow_info = it->second;
        /* fe can already exist. In that case it won't be inserted */
        pair<FlowTable::FlowEntryTree::iterator, bool> ret = 
                                            vn_flow_info->fet.insert(fe);
        if (ret.second) {
            IncrVnFlowCounter(vn_flow_info, fe);
        }
    }
}

void FlowTable::VnFlowCounters(const VnEntry *vn, uint32_t *in_count, 
                               uint32_t *out_count) {
    VnFlowTree::iterator it;
    it = vn_flow_tree_.find(vn);
    if (it == vn_flow_tree_.end()) {
        *in_count = 0;
        *out_count = 0;
        return;
    }
    VnFlowInfo *vn_flow_info = it->second;
    *in_count = vn_flow_info->ingress_flow_count;
    *out_count = vn_flow_info->egress_flow_count;
}

uint32_t FlowTable::VmFlowCount(const VmEntry *vm) {
    VmFlowTree::iterator it = vm_flow_tree_.find(vm);
    if (it != vm_flow_tree_.end()) {
        VmFlowInfo *vm_flow_info = it->second;
        return vm_flow_info->fet.size();
    }

    return 0;
}

uint32_t FlowTable::VmLinkLocalFlowCount(const VmEntry *vm) {
    VmFlowTree::iterator it = vm_flow_tree_.find(vm);
    if (it != vm_flow_tree_.end()) {
        VmFlowInfo *vm_flow_info = it->second;
        return vm_flow_info->linklocal_flow_count;
    }

    return 0;
}

void FlowTable::AddRouteFlowInfo (FlowEntry *fe)
{
    RouteFlowTree::iterator it;
    RouteFlowInfo *route_flow_info;
    if (fe->data().flow_source_vrf != VrfEntry::kInvalidIndex) {
        RouteFlowKey skey(fe->data().flow_source_vrf, fe->key().src.ipv4,
                          fe->data().source_plen);
        it = route_flow_tree_.find(skey);
        if (it == route_flow_tree_.end()) {
            route_flow_info = new RouteFlowInfo();
            route_flow_info->fet.insert(fe);
            route_flow_tree_.insert(RouteFlowPair(skey, route_flow_info));
        } else {
            route_flow_info = it->second;
            route_flow_info->fet.insert(fe);
        }
    }

    if (fe->data().flow_dest_vrf != VrfEntry::kInvalidIndex) {
        RouteFlowKey dkey(fe->data().flow_dest_vrf, fe->key().dst.ipv4, 
                          fe->data().dest_plen);
        it = route_flow_tree_.find(dkey);
        if (it == route_flow_tree_.end()) {
            route_flow_info = new RouteFlowInfo();
            route_flow_info->fet.insert(fe);
            route_flow_tree_.insert(RouteFlowPair(dkey, route_flow_info));
        } else {
            route_flow_info = it->second;
            route_flow_info->fet.insert(fe);
        }
    }
}

void FlowTable::ResyncAFlow(FlowEntry *fe) {
    fe->DoPolicy();
    fe->UpdateKSync();

    // If this is forward flow, update the SG action for reflexive entry
    if (fe->is_flags_set(FlowEntry::ReverseFlow)) {
        return;
    }

    FlowEntry *rflow = fe->reverse_flow_entry();
    if (rflow == NULL) {
        return;
    }

    rflow->UpdateReflexiveAction();
    // Check if there is change in action for reverse flow
    rflow->ActionRecompute();

    rflow->UpdateKSync();
}

void FlowTable::DeleteVnFlows(const VnEntry *vn)
{
    VnFlowTree::iterator vn_it;
    vn_it = vn_flow_tree_.find(vn);
    if (vn_it == vn_flow_tree_.end()) {
        return;
    }
    FLOW_TRACE(ModuleInfo, "Delete Vn Flows");
    FlowEntryTree fet = vn_it->second->fet;
    FlowEntryTree::iterator fet_it;
    for (fet_it = fet.begin(); fet_it != fet.end(); ++fet_it) {
        Delete((*fet_it)->key(), true);
    }
}

void FlowTable::DeleteVmFlows(const VmEntry *vm)
{
    VmFlowTree::iterator vm_it;
    vm_it = vm_flow_tree_.find(vm);
    if (vm_it == vm_flow_tree_.end()) {
        return;
    }
    FLOW_TRACE(ModuleInfo, "Delete VM flows");
    FlowEntryTree fet = vm_it->second->fet;
    FlowEntryTree::iterator fet_it;
    for (fet_it = fet.begin(); fet_it != fet.end(); ++fet_it) {
        Delete((*fet_it)->key(), true);
    }
}

void FlowTable::DeleteVmIntfFlows(const Interface *intf)
{
    IntfFlowTree::iterator intf_it;
    intf_it = intf_flow_tree_.find(intf);
    if (intf_it == intf_flow_tree_.end()) {
        return;
    }
    FLOW_TRACE(ModuleInfo, "Delete Interface Flows");
    FlowEntryTree fet = intf_it->second->fet;
    FlowEntryTree::iterator fet_it;
    for (fet_it = fet.begin(); fet_it != fet.end(); ++fet_it) {
        Delete((*fet_it)->key(), true);
    }
}

DBTableBase::ListenerId FlowTable::nh_listener_id() {
    return nh_listener_->id();
}

Inet4UnicastRouteEntry * FlowTable::GetUcRoute(const VrfEntry *entry,
        const Ip4Address &addr) {
    route_key_.set_addr(addr);
    Inet4UnicastRouteEntry *rt = entry->GetUcRoute(route_key_);
    if (rt != NULL && rt->IsRPFInvalid()) {
        return NULL;
    }
    return rt;
}

void SetActionStr(const FlowAction &action_info, std::vector<ActionStr> &action_str_l)
{
    std::bitset<32> bs(action_info.action);
    for (unsigned int i = 0; i <= bs.size(); i++) {
        if (bs[i]) {
            ActionStr astr;
            astr.action = TrafficAction::ActionToString((TrafficAction::Action)i);
            action_str_l.push_back(astr);
            if ((TrafficAction::Action)i == TrafficAction::MIRROR) {
                std::vector<MirrorActionSpec>::const_iterator m_it;
                for (m_it = action_info.mirror_l.begin();
                     m_it != action_info.mirror_l.end();
                     ++m_it) {
                    ActionStr mstr;
                    mstr.action += (*m_it).ip.to_string();
                    mstr.action += " ";
                    mstr.action += integerToString((*m_it).port);
                    mstr.action += " ";
                    mstr.action += (*m_it).vrf_name;
                    mstr.action += " ";
                    mstr.action += (*m_it).encap;
                    action_str_l.push_back(mstr);
                }
            }
        }
    }
}

void GetFlowSandeshActionParams(const FlowAction &action_info,
    std::string &action_str) {
    std::bitset<32> bs(action_info.action);
    for (unsigned int i = 0; i <= bs.size(); i++) {
        if (bs[i]) {
            if (!action_str.empty()) {
                action_str += "|";
            }
            action_str += TrafficAction::ActionToString(
                static_cast<TrafficAction::Action>(i));
        }
    } 
}

static void SetAclListAclAction(const std::list<MatchAclParams> &acl_l, std::vector<AclAction> &acl_action_l,
                         std::string &acl_type) {
    std::list<MatchAclParams>::const_iterator it;
    for(it = acl_l.begin(); it != acl_l.end(); ++it) {
        AclAction acl_action;
        acl_action.set_acl_id(UuidToString((*it).acl->GetUuid()));
        acl_action.set_acl_type(acl_type);
        std::vector<ActionStr> action_str_l;
        SetActionStr((*it).action_info, action_str_l);
        acl_action.set_action_l(action_str_l);
        acl_action_l.push_back(acl_action);
    }
}

void FlowEntry::SetAclAction(std::vector<AclAction> &acl_action_l) const
{
    const std::list<MatchAclParams> &acl_l = data_.match_p.m_acl_l;
    std::string acl_type("nw policy");
    SetAclListAclAction(acl_l, acl_action_l, acl_type);

    const std::list<MatchAclParams> &sg_acl_l = data_.match_p.m_sg_acl_l;
    acl_type = "sg";
    SetAclListAclAction(sg_acl_l, acl_action_l, acl_type);

    const std::list<MatchAclParams> &m_acl_l = data_.match_p.m_mirror_acl_l;
    acl_type = "dynamic";
    SetAclListAclAction(m_acl_l, acl_action_l, acl_type);

    const std::list<MatchAclParams> &out_acl_l = data_.match_p.m_out_acl_l;
    acl_type = "o nw policy";
    SetAclListAclAction(out_acl_l, acl_action_l, acl_type);

    const std::list<MatchAclParams> &out_sg_acl_l = data_.match_p.m_out_sg_acl_l;
    acl_type = "o sg";
    SetAclListAclAction(out_sg_acl_l, acl_action_l, acl_type);

    const std::list<MatchAclParams> &out_m_acl_l = data_.match_p.m_out_mirror_acl_l;
    acl_type = "o dynamic";
    SetAclListAclAction(out_m_acl_l, acl_action_l, acl_type);

    const std::list<MatchAclParams> &r_sg_l = data_.match_p.m_reverse_sg_acl_l;
    acl_type = "r sg";
    SetAclListAclAction(r_sg_l, acl_action_l, acl_type);

    const std::list<MatchAclParams> &r_out_sg_l = data_.match_p.m_reverse_out_sg_acl_l;
    acl_type = "r o sg";
    SetAclListAclAction(r_out_sg_l, acl_action_l, acl_type);
}

uint32_t FlowEntry::reverse_flow_fip() const {
    FlowEntry *rflow = reverse_flow_entry_.get();
    if (rflow) {
        return rflow->stats().fip;
    }
    return 0;
}

uint32_t FlowEntry::reverse_flow_vmport_id() const {
    FlowEntry *rflow = reverse_flow_entry_.get();
    if (rflow) {
        return rflow->stats().fip_vm_port_id;
    }
    return Interface::kInvalidIndex;
}

string FlowTable::GetAceSandeshDataKey(const AclDBEntry *acl, int ace_id) {
    string uuid_str = UuidToString(acl->GetUuid());
    stringstream ss;
    ss << uuid_str << ":";
    ss << ace_id;
    return ss.str();
}

void FlowTable::SetAceSandeshData(const AclDBEntry *acl, AclFlowCountResp &data, int ace_id)
{
    AclFlowTree::iterator it;
    it = acl_flow_tree_.find(acl);
    if (it == acl_flow_tree_.end()) {
        return;
    }
    int count = 0;
    bool key_set = false;
    AclFlowInfo *af_info = it->second;

    AceIdFlowCntMap *aceid_cnt = &(af_info->aceid_cnt_map);
    FlowTable::AceIdFlowCntMap::iterator aceid_it = aceid_cnt->upper_bound(ace_id);
    std::vector<AceIdFlowCnt> id_cnt_l;
    while (aceid_it != aceid_cnt->end()) {
        AceIdFlowCnt id_cnt_s;
        id_cnt_s.ace_id = aceid_it->first;
        id_cnt_s.flow_cnt = aceid_it->second;
        id_cnt_l.push_back(id_cnt_s);
        count++;
        ++aceid_it;
        if (count == MaxResponses && aceid_it != aceid_cnt->end()) {
            data.set_iteration_key(GetAceSandeshDataKey(acl, id_cnt_s.ace_id));
            key_set = true;
            break;
        }
    }
    data.set_aceid_cnt_list(id_cnt_l);
    
    FlowEntryTree *fe_tree = &(af_info->fet);    
    data.set_flow_count(fe_tree->size());
    data.set_flow_miss(af_info->flow_miss);

    if (!key_set) {
        data.set_iteration_key(GetAceSandeshDataKey(acl, 0));
    }
}

string FlowTable::GetAclFlowSandeshDataKey(const AclDBEntry *acl, const int last_count) {
    string uuid_str = UuidToString(acl->GetUuid());
    stringstream ss;
    ss << uuid_str << ":";
    ss << last_count;
    return ss.str();
}

void FlowTable::SetAclFlowSandeshData(const AclDBEntry *acl, AclFlowResp &data, 
                                      const int last_count)
{
    AclFlowTree::iterator it;
    it = acl_flow_tree_.find(acl);
    if (it == acl_flow_tree_.end()) {
        return;
    }
    AclFlowInfo *af_info = it->second;
   
    int count = 0; 
    bool key_set = false;
    FlowEntryTree *fe_tree = &(af_info->fet);    
    FlowEntryTree::iterator fe_tree_it = fe_tree->begin();
    while (fe_tree_it != fe_tree->end() && (count + 1) < last_count) {
        fe_tree_it++;
        count++;
    }
    data.set_flow_count(fe_tree->size());
    data.set_flow_miss(af_info->flow_miss);
    std::vector<FlowSandeshData> flow_entries_l;
    while(fe_tree_it != fe_tree->end()) {
        const FlowEntry &fe = *(*fe_tree_it);
        FlowSandeshData fe_sandesh_data;
        fe.SetAclFlowSandeshData(acl, fe_sandesh_data);

        flow_entries_l.push_back(fe_sandesh_data);
        count++;
        ++fe_tree_it;
        if (count == (MaxResponses + last_count) && fe_tree_it != fe_tree->end()) {
            data.set_iteration_key(GetAclFlowSandeshDataKey(acl, count));
            key_set = true;
            break;
        }
    }
    data.set_flow_entries(flow_entries_l);
    if (!key_set) {
        data.set_iteration_key(GetAclFlowSandeshDataKey(acl, 0));
    }
}

FlowTable::FlowTable(Agent *agent) : 
    agent_(agent), flow_entry_map_(), acl_flow_tree_(),
    linklocal_flow_count_(), acl_listener_id_(),
    intf_listener_id_(), vn_listener_id_(), vm_listener_id_(),
    vrf_listener_id_(), nh_listener_(NULL),
    route_key_(NULL, Ip4Address(), 32, false) {
    max_vm_flows_ =
        (agent->ksync()->flowtable_ksync_obj()->flow_table_entries_count() *
        (uint32_t) agent->params()->max_vm_flows()) / 100;
}

FlowTable::~FlowTable() {
    agent_->GetAclTable()->Unregister(acl_listener_id_);
    agent_->GetInterfaceTable()->Unregister(intf_listener_id_);
    agent_->GetVnTable()->Unregister(vn_listener_id_);
    agent_->GetVmTable()->Unregister(vm_listener_id_);
    agent_->GetVrfTable()->Unregister(vrf_listener_id_);
    delete nh_listener_;
}

