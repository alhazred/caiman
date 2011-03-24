#!/usr/bin/python
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
# Copyright (c) 2011, Oracle and/or its affiliates. All rights reserved.
#

'''
Class and functions supporting representing a NIC and locating
the NICs installed on the system by name (using dladm)
'''

import logging
from subprocess import Popen, PIPE

from solaris_install.data_object import DataObject
from solaris_install.logger import INSTALL_LOGGER_NAME
from solaris_install.sysconfig.profile.ip_address import IPAddress
from solaris_install.sysconfig.profile import SMFConfig, SMFInstance, \
     SMFPropertyGroup, NETWORK_LABEL


_LOGGER = None


def LOGGER():
    global _LOGGER
    if _LOGGER is None:
        _LOGGER = logging.getLogger(INSTALL_LOGGER_NAME + ".sysconfig")
    return _LOGGER


class NetworkInfo(SMFConfig):
    '''Represents a NIC and its network settings'''
    
    AUTOMATIC = "automatic"
    MANUAL = "manual"
    NONE = "none"
    DEFAULT_NETMASK = "255.255.255.0"
    
    ETHER_NICS = None
    
    LABEL = NETWORK_LABEL
    
    @staticmethod
    def find_links():
        '''Use dladm show-link to find the physical links
        on the system
        
        '''
        
        if NetworkInfo.ETHER_NICS is not None:
            return NetworkInfo.ETHER_NICS
        
        argslist = ['/usr/sbin/dladm', 'show-link', '-o', 'link', '-p']
        
        try:
            (nic_list, dladm_err) = Popen(argslist, stdout=PIPE,
                                          stderr=PIPE).communicate()
        except OSError, err:
            LOGGER().warn("OSError occurred: %s", err)
            return []
        if dladm_err:
            LOGGER().warn("Error occurred during call to dladm: %s", dladm_err)
        # pylint: disable-msg=E1103
        # nic_list is a string
        NetworkInfo.ETHER_NICS = nic_list.splitlines()
        NetworkInfo.ETHER_NICS.sort()
        return NetworkInfo.ETHER_NICS

    def __init__(self, nic_name=None, net_type=None, ip_address=None,
                 netmask=None, gateway=None, dns_address=None, domain=None):
        DataObject.__init__(self, self.LABEL)
        
        self.nic_name = nic_name
        self.type = net_type
        self.ip_address = ip_address
        if netmask is None:
            netmask = NetworkInfo.DEFAULT_NETMASK
        self.netmask = netmask
        self.gateway = gateway
        self.dns_address = dns_address
        self.domain = domain
        self.find_defaults = True
    
    def __repr__(self):
        result = ["NIC %s:" % self.nic_name]
        result.append("Type: %s" % self.type)
        if self.type == NetworkInfo.MANUAL:
            result.append("IP: %s" % self.ip_address)
            result.append("Netmask: %s" % self.netmask)
            result.append("Router: %s" % self.gateway)
            result.append("DNS: %s" % self.dns_address)
            result.append("Domain: %s" % self.domain)
        return "\n".join(result)

    # pylint: disable-msg=E0202
    @property
    def type(self):
        return self._type

    # pylint: disable-msg=E1101
    # pylint: disable-msg=E0102
    # pylint: disable-msg=E0202
    @type.setter
    def type(self, value):
        if value not in [NetworkInfo.AUTOMATIC, NetworkInfo.MANUAL,
                         NetworkInfo.NONE, None]:
            raise ValueError("'%s' is an invalid type."
                             "Must be one of %s" % (value,
                             [NetworkInfo.AUTOMATIC, NetworkInfo.MANUAL,
                              NetworkInfo.NONE]))
        # pylint: disable-msg=W0201
        self._type = value
    
    def find_dns(self):
        '''Try to determine the DNS info of the NIC if DHCP is running
        Returns True if this action was successful
        
        '''
        dns_server = self._run_dhcpinfo("DNSserv")
        if dns_server:
            self.dns_address = dns_server
            return True
        else:
            return False
    
    def find_gateway(self):
        '''Try to determine the router of the NIC if DHCP is running
        Returns True if this action was successful
        
        '''
        gateway = self._run_dhcpinfo("Router")
        if gateway:
            self.gateway = gateway
            return True
        else:
            return False
    
    def find_domain(self):
        '''Try to determine the domain info of the NIC if DHCP is running
        Returns True if this action was successful
        
        '''
        domain = self._run_dhcpinfo("DNSdmain")
        if domain:
            self.domain = domain
            return True
        else:
            return False
    
    def find_netmask(self):
        '''Try to determine the netmask info of the NIC if DHCP is running
        Returns True if this action was successful
        
        '''
        netmask = self._run_dhcpinfo("Subnet")
        if netmask:
            self.netmask = IPAddress(netmask)
            return True
        else:
            return False
    
    def get_ifconfig_data(self):
        '''Returns a dictionary populated with the data returned from ifconfig
        Returns None if the call to ifconfig fails in some way
        
        '''
        argslist = ['/sbin/ifconfig', self.nic_name]
        try:
            (ifconfig_out, ifconfig_err) = Popen(argslist, stdout=PIPE,
                                                 stderr=PIPE).communicate()
        except OSError, err:
            LOGGER().warn("Failed to call ifconfig: %s", err)
            return None
        if ifconfig_err:
            LOGGER().warn("Error occurred during call to ifconfig: %s",
                         ifconfig_err)
            return None
        # pylint: disable-msg=E1103
        # ifconfig_out is a string
        ifconfig_out = ifconfig_out.split()
        link_data = {}
        link_data['flags'] = ifconfig_out[1]
        ifconfig_out = ifconfig_out[2:]
        for i in range(len(ifconfig_out) / 2):
            link_data[ifconfig_out[2*i]] = ifconfig_out[2*i+1]
        return link_data
    
    def _run_dhcpinfo(self, code):
        '''Run the dhcpinfo command against this NIC, requesting 'code'
        
        This function always returns successfully; if the underlying call
        to dhcpinfo fails, then None is returned.
        '''
        ifconfig_data = self.get_ifconfig_data()
        if not ifconfig_data or ifconfig_data['flags'].count("DHCP") == 0:
            LOGGER().warn("This connection not using DHCP")
            return None
        
        argslist = ['/sbin/dhcpinfo',
                    '-i', self.nic_name,
                    '-n', '1',
                    code]
        try:
            (dhcpout, dhcperr) = Popen(argslist, stdout=PIPE,
                                       stderr=PIPE).communicate()
        except OSError, err:
            LOGGER().warn("OSError ocurred during dhcpinfo call: %s", err)
            return None
        
        if dhcperr:
            LOGGER().warn("Error ocurred during dhcpinfo call: %s", dhcperr)
        
        # pylint: disable-msg=E1103
        # dhcpout is a string
        return dhcpout.rstrip("\n")
    
    def to_xml(self):
        data_objects = []
        
        net_physical = SMFConfig("network/physical")
        data_objects.append(net_physical)
        
        nwam = SMFInstance("nwam", enabled=False)
        net_default = SMFInstance("default", enabled=True)
        net_physical.insert_children([nwam, net_default])
        
        if self.type == NetworkInfo.AUTOMATIC:
            nwam.enabled = True
            net_default.enabled = False
        elif self.type == NetworkInfo.MANUAL:
            net_install = SMFConfig('network/install')
            data_objects.append(net_install)

            net_install_default = SMFInstance("default", enabled=True)
            net_install.insert_children([net_install_default])

            ipv4 = SMFPropertyGroup('install_ipv4_interface')
            ipv6 = SMFPropertyGroup('install_ipv6_interface')
            net_install_default.insert_children([ipv4, ipv6])
            
            static_address = IPAddress(self.ip_address, netmask=self.netmask)

            # IPv4 configuration
            ipv4.add_props(static_address=static_address,
                           name='%s/v4' % self.nic_name,
                           address_type='static')

            #
            # IPv4 default route is optional. If it was not configured
            # on Network screen, do not populate related smf property.
            #
            if self.gateway:
                ipv4.add_props(default_route=self.gateway)

            # IPv6 configuration
            ipv6.add_props(name='%s/v6' % self.nic_name,
                           address_type='addrconf',
                           stateless='yes',
                           stateful='yes')
            
            #
            # If neither DNS nameservers nor domain was provided,
            # there is nothing to be configured for DNS.
            #
            if self.dns_address or self.domain:
                dns = SMFConfig('network/dns/install')
                data_objects.append(dns)
            
                dns_default = SMFInstance('default')
                dns.insert_children([dns_default])
                dns_props = SMFPropertyGroup('install_props')
                dns_default.insert_children([dns_props])

                # configure DNS nameservers
                if self.dns_address:
                    nameserver = dns_props.setprop("property", "nameserver",
                                               "net_address_v4")
                    nameserver.add_value_list(propvals=[self.dns_address])

                # configure DNS domain
                if self.domain:
                    search = dns_props.setprop("property", "search", "astring")
                    search.add_value_list(propvals=[self.domain])

                # enable dns/client smf service
                dns_client = SMFConfig('network/dns/client')
                data_objects.append(dns_client)
                dns_client_default = SMFInstance('default')
                dns_client.insert_children([dns_client_default])

        return [do.get_xml_tree() for do in data_objects]

    @classmethod
    def from_xml(cls, xml_node):
        return None
    
    @classmethod
    def can_handle(cls, xml_node):
        return False