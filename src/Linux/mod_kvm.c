/* This software is distributed under the following license:
 * http://sflow.net/license.html
 */

#if defined(__cplusplus)
extern "C" {
#endif

#include "hsflowd.h"

#include "libvirt.h"
#include "libxml/xmlreader.h"

  typedef struct _HSPVMState_KVM {
    HSPVMState vm; // superclass: must come first
    int virDomainId;
    char nova_name[100];
  } HSPVMState_KVM;

  typedef struct _HSP_mod_KVM {
    virConnectPtr virConn;
    UTHash *vmsByUUID;
    UTArray *pollActions;
    SFLCounters_sample_element vnodeElem;
    int num_domains;
    uint32_t refreshVMListSecs;
    time_t next_refreshVMList;
    uint32_t forgetVMSecs;
  } HSP_mod_KVM;

  static void agentCB_getCounters_KVM(void *magic, SFLPoller *poller, SFL_COUNTERS_SAMPLE_TYPE *cs)
  {
    EVMod *mod = (EVMod *)magic;
    HSP_mod_KVM *mdata = (HSP_mod_KVM *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);

    assert(poller->magic);
    HSPVMState_KVM *state = (HSPVMState_KVM *)poller->userData;
    HSPVMState *vm = (HSPVMState *)&state->vm;
    if(state == NULL) {
      myDebug(1, "agentCB_getCounters_KVM: state==NULL");
      return;
    }

    if(mdata->virConn) {
      virDomainPtr domainPtr = virDomainLookupByID(mdata->virConn, state->virDomainId);
      if(domainPtr == NULL) {
	sp->refreshVMList = YES;
      }
      else {
	// host ID
	SFLCounters_sample_element hidElem = { 0 };
	hidElem.tag = SFLCOUNTERS_HOST_HID;
        const char *hname;
        if ( *state->nova_name ) {
	  hname = state->nova_name; // no need to free this one
          myDebug(1, "agentCB_getCounters_KVM: use nova_name: %s", hname);
        } else {
	  hname = virDomainGetName(domainPtr); // no need to free this one
          myDebug(1, "agentCB_getCounters_KVM: use instance name: %s", hname);
        }

	if(hname) {
	  // copy the name out here so we can free it straight away
	  hidElem.counterBlock.host_hid.hostname.str = (char *)hname;
	  hidElem.counterBlock.host_hid.hostname.len = strlen(hname);
	  virDomainGetUUID(domainPtr, hidElem.counterBlock.host_hid.uuid);

	  // char *osType = virDomainGetOSType(domainPtr); $$$
	  hidElem.counterBlock.host_hid.machine_type = SFLMT_unknown;//$$$
	  hidElem.counterBlock.host_hid.os_name = SFLOS_unknown;//$$$
	  //hidElem.counterBlock.host_hid.os_release.str = NULL;
	  //hidElem.counterBlock.host_hid.os_release.len = 0;
	  SFLADD_ELEMENT(cs, &hidElem);
	}

	// host parent
	SFLCounters_sample_element parElem = { 0 };
	parElem.tag = SFLCOUNTERS_HOST_PAR;
	parElem.counterBlock.host_par.dsClass = SFL_DSCLASS_PHYSICAL_ENTITY;
	parElem.counterBlock.host_par.dsIndex = HSP_DEFAULT_PHYSICAL_DSINDEX;
	SFLADD_ELEMENT(cs, &parElem);

	// VM Net I/O
	SFLCounters_sample_element nioElem = { 0 };
	nioElem.tag = SFLCOUNTERS_HOST_VRT_NIO;
	// since we are already maintaining the accumulated network counters (and handling issues like 32-bit
	// rollover) then we can just use the same mechanism again.  On a non-linux platform we may
	// want to take advantage of the libvirt call to get the counters (it takes the domain id and the
	// device name as parameters so you have to call it multiple times),  but even then we would
	// probably do that down inside the readNioCounters() fn in case there is work to do on the
	// accumulation and rollover-detection.
	readNioCounters(sp, (SFLHost_nio_counters *)&nioElem.counterBlock.host_vrt_nio, NULL, vm->interfaces);
	SFLADD_ELEMENT(cs, &nioElem);

	// VM cpu counters [ref xenstat.c]
	SFLCounters_sample_element cpuElem = { 0 };
	cpuElem.tag = SFLCOUNTERS_HOST_VRT_CPU;
	virDomainInfo domainInfo;
	int domainInfoOK = NO;
	if(virDomainGetInfo(domainPtr, &domainInfo) != 0) {
	  myLog(LOG_ERR, "virDomainGetInfo() failed");
	}
	else {
	  domainInfoOK = YES;
	  // enum virDomainState really is the same as enum SFLVirDomainState
	  cpuElem.counterBlock.host_vrt_cpu.state = domainInfo.state;
	  cpuElem.counterBlock.host_vrt_cpu.cpuTime = (domainInfo.cpuTime / 1000000);
	  cpuElem.counterBlock.host_vrt_cpu.nrVirtCpu = domainInfo.nrVirtCpu;
	  SFLADD_ELEMENT(cs, &cpuElem);
	}

	SFLCounters_sample_element memElem = { 0 };
	memElem.tag = SFLCOUNTERS_HOST_VRT_MEM;
	if(domainInfoOK) {
	  memElem.counterBlock.host_vrt_mem.memory = domainInfo.memory * 1024;
	  memElem.counterBlock.host_vrt_mem.maxMemory = (domainInfo.maxMem == UINT_MAX) ? -1 : (domainInfo.maxMem * 1024);
	  SFLADD_ELEMENT(cs, &memElem);
	}

	// VM disk I/O counters
	SFLCounters_sample_element dskElem = { 0 };
	dskElem.tag = SFLCOUNTERS_HOST_VRT_DSK;
	for(int i = strArrayN(vm->disks); --i >= 0; ) {
	  /* vm->volumes and vm->disks are populated in lockstep
	   * so they always have the same number of elements
	   */
	  char *volPath = strArrayAt(vm->volumes, i);
	  char *dskPath = strArrayAt(vm->disks, i);
	  bool gotVolInfo = NO;

#if (LIBVIR_VERSION_NUMBER >= 8001)
	  if(gotVolInfo == NO) {
	    /* try appealing directly to the disk path instead */
	    /* this call was only added in April 2010 (version 0.8.1).
	     * See http://markmail.org/message/mjafgt47f5e5zzfc
	     */
	    virDomainBlockInfo blkInfo;
	    if(virDomainGetBlockInfo(domainPtr, volPath, &blkInfo, 0) == -1) {
	      myLog(LOG_ERR, "virDomainGetBlockInfo(%s) failed", dskPath);
	    }
	    else {
	      dskElem.counterBlock.host_vrt_dsk.capacity += blkInfo.capacity;
	      dskElem.counterBlock.host_vrt_dsk.allocation += blkInfo.allocation;
	      dskElem.counterBlock.host_vrt_dsk.available += (blkInfo.capacity - blkInfo.allocation);
	      // don't need blkInfo.physical
	      gotVolInfo = YES;
	    }
	  }
#endif

	  if(gotVolInfo == NO) {
	    virStorageVolPtr volPtr = virStorageVolLookupByPath(mdata->virConn, volPath);
	    if(volPtr == NULL) {
	      myLog(LOG_ERR, "virStorageLookupByPath(%s) failed", volPath);
	    }
	    else {
	      virStorageVolInfo volInfo;
	      if(virStorageVolGetInfo(volPtr, &volInfo) != 0) {
		myLog(LOG_ERR, "virStorageVolGetInfo(%s) failed", volPath);
	      }
	      else {
		gotVolInfo = YES;
		dskElem.counterBlock.host_vrt_dsk.capacity += volInfo.capacity;
		dskElem.counterBlock.host_vrt_dsk.allocation += volInfo.allocation;
		dskElem.counterBlock.host_vrt_dsk.available += (volInfo.capacity - volInfo.allocation);
	      }
	    }
	  }

	  /* we get reads, writes and errors from a different call */
	  virDomainBlockStatsStruct blkStats;
	  if(virDomainBlockStats(domainPtr, dskPath, &blkStats, sizeof(blkStats)) != -1) {
	    if(blkStats.rd_req != -1) dskElem.counterBlock.host_vrt_dsk.rd_req += blkStats.rd_req;
	    if(blkStats.rd_bytes != -1) dskElem.counterBlock.host_vrt_dsk.rd_bytes += blkStats.rd_bytes;
	    if(blkStats.wr_req != -1) dskElem.counterBlock.host_vrt_dsk.wr_req += blkStats.wr_req;
	    if(blkStats.wr_bytes != -1) dskElem.counterBlock.host_vrt_dsk.wr_bytes += blkStats.wr_bytes;
	    if(blkStats.errs != -1) dskElem.counterBlock.host_vrt_dsk.errs += blkStats.errs;
	  }
	}
	SFLADD_ELEMENT(cs, &dskElem);

	// include my slice of the adaptor list
	SFLCounters_sample_element adaptorsElem = { 0 };
	adaptorsElem.tag = SFLCOUNTERS_ADAPTORS;
	adaptorsElem.counterBlock.adaptors = vm->interfaces;
	SFLADD_ELEMENT(cs, &adaptorsElem);

	SEMLOCK_DO(sp->sync_agent) {
	  sfl_poller_writeCountersSample(poller, cs);
	  sp->counterSampleQueued = YES;
	  sp->telemetry[HSP_TELEMETRY_COUNTER_SAMPLES]++;
	}

	virDomainFree(domainPtr);
      }
    }
  }

  /*_________________--------------------------------------__________________
    _________________  sflow agent callback for counters   __________________
    -----------------______________________________________------------------
  */

  static void agentCB_getCounters_KVM_request(void *magic, SFLPoller *poller, SFL_COUNTERS_SAMPLE_TYPE *cs)
  {
    EVMod *mod = (EVMod *)poller->magic;
    HSP_mod_KVM *mdata = (HSP_mod_KVM *)mod->data;
    // defer, since the agent mutex is currently held and we don't want to block it
    UTArrayAdd(mdata->pollActions, poller);
  }

  /*_________________---------------------------__________________
    _________________    domain_xml_node        __________________
    -----------------___________________________------------------
  */

  static int domain_xml_path_equal(xmlNode *node, char *nodeName, ...) {
    myDebug(1, "Parse xml node %s", (char *)node->name);
    if(node == NULL
       || node->name == NULL
       || node->type != XML_ELEMENT_NODE
       || !my_strequal(nodeName, (char *)node->name)) {
      return NO;
    }
    int match = YES;
    va_list names;
    va_start(names, nodeName);
    xmlNode *parentNode = node->parent;
    char *parentName;
    while((parentName = va_arg(names, char *)) != NULL) {
      if(parentNode == NULL
	 || parentNode->name == NULL
	 || !my_strequal(parentName, (char *)parentNode->name)) {
	match = NO;
	break;
      }
      parentNode = parentNode->parent;
    }
    va_end(names);
    return match;
  }

  static char *get_xml_attr(xmlNode *node, char *attrName) {
    for(xmlAttr *attr = node->properties; attr; attr = attr->next) {
      if(attr->name) {
	myDebug(1, "attribute %s", attr->name);
	if(attr->children && !strcmp((char *)attr->name, attrName)) {
	  return (char *)attr->children->content;
	}
      }
    }
    return NULL;
  }

  static void domain_xml_interface(xmlNode *node, char **ifname, char **ifmac) {
    for(xmlNode *n = node; n; n = n->next) {
      if(domain_xml_path_equal(n, "target", "interface", "devices", NULL)) {
	char *dev = get_xml_attr(n, "dev");
	if(dev) {
	  myDebug(1, "interface.dev=%s", dev);
	  if(ifname) *ifname = dev;
	}
      }
      else if(domain_xml_path_equal(n, "mac", "interface", "devices", NULL)) {
	char *addr = get_xml_attr(n, "address");
	myDebug(1, "interface.mac=%s", addr);
	if(ifmac) *ifmac = addr;
      }
    }
    if(node->children) domain_xml_interface(node->children, ifname, ifmac);
  }

  static void domain_xml_disk(xmlNode *node, char **disk_path, char **disk_dev) {
    for(xmlNode *n = node; n; n = n->next) {
      if(domain_xml_path_equal(n, "source", "disk", "devices", NULL)) {
	char *path = get_xml_attr(n, "file");
	if(path) {
	  myDebug(1, "disk.file=%s", path);
	  if(disk_path) *disk_path = path;
	}
      }
      else if(domain_xml_path_equal(n, "target", "disk", "devices", NULL)) {
	char *dev = get_xml_attr(n, "dev");
	myDebug(1, "disk.dev=%s", dev);
	if(disk_dev) *disk_dev = dev;
      }
      else if(domain_xml_path_equal(n, "readonly", "disk", "devices", NULL)) {
	myDebug(1, "ignoring readonly device");
	*disk_path = NULL;
	*disk_dev = NULL;
	return;
      }
    }
    if(node->children) domain_xml_disk(node->children, disk_path, disk_dev);
  }

  static void domain_xml_nova_name(xmlNode *n, HSPVMState_KVM *state) {
    if (domain_xml_path_equal(n, "name", "instance", "metadata", "domain", NULL)) {
      xmlChar *nova_name;
      nova_name = xmlNodeGetContent(n);
      if (nova_name) {
	myDebug(1, "domain_xml_nova_name: extract name: %s", nova_name);
        strcpy(state->nova_name, (const char *) nova_name);
        xmlFree(nova_name);
      } 
    }
  }

  static void domain_xml_node(HSP *sp, xmlNode *node, HSPVMState_KVM *state) {
    for(xmlNode *n = node; n; n = n->next) {
      domain_xml_nova_name(n, state);
      if(domain_xml_path_equal(n, "interface", "devices", "domain", NULL)) {
	char *ifname=NULL,*ifmac=NULL;
	domain_xml_interface(n, &ifname, &ifmac);
	if(ifname && ifmac) {
	  SFLMacAddress mac;
	  memset(&mac, 0, sizeof(mac));
	  if(hexToBinary((u_char *)ifmac, mac.mac, 6) == 6) {
	    SFLAdaptor *adaptor = adaptorListGet(state->vm.interfaces, ifname);
	    if(adaptor == NULL) {
	      // allocate my own  adaptors so it's safe to free them later
	      adaptor = nioAdaptorNew(ifname, mac.mac, 0);
	      SFLAdaptor *global_ad = adaptorByMac(sp, &mac);
	      if(global_ad) {
		// copy index numbers to my private copy
		adaptor->ifIndex = global_ad->ifIndex;
		adaptor->peer_ifIndex = global_ad->peer_ifIndex;
	      }
	      else {
		// not in global collection - add
		// This may be a mistake since ifIndex is unknown
		if(UTHashAdd(sp->adaptorsByMac, adaptor) != NULL)
		  myDebug(1, "Warning: kvm adaptor overwriting adaptorsByMac");
	      }
	      adaptorListAdd(state->vm.interfaces, adaptor);
	    }
	    // mark it as a vm/container device
	    ADAPTOR_NIO(adaptor)->vm_or_container = YES;
	    // clear the mark so we don't free it
	    adaptor->marked = NO;
	  }
	}
      }
      else if(domain_xml_path_equal(n, "disk", "devices", "domain", NULL)) {
	// need both a path and a dev before we will accept it
	char *disk_path=NULL,*disk_dev=NULL;
	domain_xml_disk(n, &disk_path, &disk_dev);
	if(disk_path && disk_dev) {
	  strArrayAdd(state->vm.volumes, (char *)disk_path);
	  strArrayAdd(state->vm.disks, (char *)disk_dev);
	}
      }
      else if(n->children) domain_xml_node(sp, n->children, state);
    }
  }

  /*_________________---------------------------__________________
    _________________   add and remove VM       __________________
    -----------------___________________________------------------
  */

  HSPVMState_KVM *getVM_KVM(EVMod *mod, char *uuid) {
    HSP_mod_KVM *mdata = (HSP_mod_KVM *)mod->data;
    HSPVMState_KVM search;
    memset(&search, 0, sizeof(search));
    memcpy(search.vm.uuid, uuid, 16);
    HSPVMState_KVM *state = UTHashGet(mdata->vmsByUUID, &search);
    if(state == NULL) {
      // new vm or container
      state = (HSPVMState_KVM *)getVM(mod, uuid, YES, sizeof(HSPVMState_KVM), VMTYPE_KVM, agentCB_getCounters_KVM_request);
      if(state) {
	UTHashAdd(mdata->vmsByUUID, state);
      }
    }
    return state;
  }

  static void removeAndFreeVM_KVM(EVMod *mod, HSPVMState_KVM *state) {
    HSP_mod_KVM *mdata = (HSP_mod_KVM *)mod->data;
    myDebug(1, "removeAndFreeVM: removing vm with dsIndex=%u (domId=%u)",
	  state->vm.dsIndex,
	  state->virDomainId);
    UTHashDel(mdata->vmsByUUID, state);
    HSPVMState *vm = &state->vm;
    removeAndFreeVM(mod, vm);
  }

  /*_________________---------------------------__________________
    _________________    configVMs_KVM          __________________
    -----------------___________________________------------------
  */

  static void configVMs_KVM(EVMod *mod) {
    HSP_mod_KVM *mdata = (HSP_mod_KVM *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);
    if(mdata->virConn == NULL) {
      // no libvirt connection
      return;
    }
    int num_domains = virConnectNumOfDomains(mdata->virConn);
    if(num_domains == -1) {
      myLog(LOG_ERR, "virConnectNumOfDomains() returned -1");
      return;
    }
    int *domainIds = (int *)my_calloc(num_domains * sizeof(int));
    if(virConnectListDomains(mdata->virConn, domainIds, num_domains) != num_domains) {
      my_free(domainIds);
      return;
    }
    for(int i = 0; i < num_domains; i++) {
      int domId = domainIds[i];
      virDomainPtr domainPtr = virDomainLookupByID(mdata->virConn, domId);
      if(domainPtr) {
	char uuid[16];
	virDomainGetUUID(domainPtr, (u_char *)uuid);
	HSPVMState_KVM *state = getVM_KVM(mod, uuid);
	HSPVMState *vm = (HSPVMState *)&state->vm;
	vm->marked = NO;
	vm->created = NO;
	// remember the domId, which might have changed (if vm rebooted)
	state->virDomainId = domId;
	// reset the information that we are about to refresh
	adaptorListMarkAll(vm->interfaces);
	strArrayReset(vm->volumes);
	strArrayReset(vm->disks);
	// get the XML descr - this seems more portable than some of
	// the newer libvert API calls,  such as those to list interfaces
	char *xmlstr = virDomainGetXMLDesc(domainPtr, 0 /*VIR_DOMAIN_XML_SECURE not allowed for read-only */);
	if(xmlstr == NULL) {
	  myLog(LOG_ERR, "virDomainGetXMLDesc(domain=%u, 0) failed", domId);
	}
	else {
	  // parse the XML to get the list of interfaces and storage nodes
	  xmlDoc *doc = xmlParseMemory(xmlstr, strlen(xmlstr));
	  if(doc) {
	    xmlNode *rootNode = xmlDocGetRootElement(doc);
	    domain_xml_node(sp, rootNode, state);
	    xmlFreeDoc(doc);
	  }
	  free(xmlstr); // allocated by virDomainGetXMLDesc()
	}
	xmlCleanupParser();
	virDomainFree(domainPtr);
	// fully delete and free the marked adaptors - some may return if
	// they are still present in the global-namespace list,  but
	// we have to do this here in case one of these was discovered
	// and allocated just for this VM.
	deleteMarkedAdaptors_adaptorList(sp, vm->interfaces);
	adaptorListFreeMarked(vm->interfaces);
      }
    }
    mdata->num_domains = num_domains;
    my_free(domainIds);
  }

  /*_________________---------------------------__________________
    _________________     getConnection         __________________
    -----------------___________________________------------------
  */

  static virConnectPtr getConnection(EVMod *mod) {
    HSP_mod_KVM *mdata = (HSP_mod_KVM *)mod->data;
    if(mdata->virConn == NULL) {
      mdata->virConn = virConnectOpenReadOnly(NULL);
      if(mdata->virConn == NULL) {
	myLog(LOG_ERR, "virConnectOpenReadOnly() failed\n");
      }
    }
    return mdata->virConn;
  }

  /*_________________---------------------------__________________
    _________________    configVMs              __________________
    -----------------___________________________------------------
  */

  static void configVMs(EVMod *mod) {
    HSP_mod_KVM *mdata = (HSP_mod_KVM *)mod->data;

    if(getConnection(mod) == NULL)
      return;

    // mark and sweep
    // 1. mark all the current virtual pollers
    HSPVMState_KVM *state;
    UTHASH_WALK(mdata->vmsByUUID, state) {
      state->vm.marked = YES;
    }

    // 2. create new VM pollers, or clear the mark on existing ones
    configVMs_KVM(mod);

    // 3. remove any VMs (and their pollers) that don't survive
    UTHASH_WALK(mdata->vmsByUUID, state) {
      if(state->vm.marked) {
	removeAndFreeVM_KVM(mod, state);
      }
    }
  }

  static void evt_tick(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    HSP_mod_KVM *mdata = (HSP_mod_KVM *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);
    time_t clk = evt->bus->now.tv_sec;
    if(clk >= mdata->next_refreshVMList
       && sp->sFlowSettings) {
      configVMs(mod);
      mdata->next_refreshVMList = clk + mdata->refreshVMListSecs;
    }
  }

  static void evt_tock(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    HSP_mod_KVM *mdata = (HSP_mod_KVM *)mod->data;
    // now we can execute pollActions without holding on to the semaphore
    for(uint32_t ii = 0; ii < UTArrayN(mdata->pollActions); ii++) {
      SFLPoller *poller = (SFLPoller *)UTArrayAt(mdata->pollActions, ii);
      SFL_COUNTERS_SAMPLE_TYPE cs;
      memset(&cs, 0, sizeof(cs));
      agentCB_getCounters_KVM((void *)mod, poller, &cs);
    }
    UTArrayReset(mdata->pollActions);
  }

  static void evt_host_cs(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    SFL_COUNTERS_SAMPLE_TYPE *cs = *(SFL_COUNTERS_SAMPLE_TYPE **)data;
    HSP_mod_KVM *mdata = (HSP_mod_KVM *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);

    if(!hasVNodeRole(mod, HSP_VNODE_PRIORITY_KVM))
      return;

    memset(&mdata->vnodeElem, 0, sizeof(mdata->vnodeElem));
    mdata->vnodeElem.tag = SFLCOUNTERS_HOST_VRT_NODE;
    mdata->vnodeElem.counterBlock.host_vrt_node.mhz = sp->cpu_mhz;
    mdata->vnodeElem.counterBlock.host_vrt_node.cpus = sp->cpu_cores;
    mdata->vnodeElem.counterBlock.host_vrt_node.num_domains = mdata->num_domains;
    mdata->vnodeElem.counterBlock.host_vrt_node.memory = sp->mem_total;
    mdata->vnodeElem.counterBlock.host_vrt_node.memory_free = sp->mem_free;
    SFLADD_ELEMENT(cs, &mdata->vnodeElem);
  }

  static void evt_final(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    HSP_mod_KVM *mdata = (HSP_mod_KVM *)mod->data;
    if(mdata->virConn) {
      virConnectClose(mdata->virConn);
      mdata->virConn = NULL;
    }
  }

  /*_________________---------------------------__________________
    _________________    module init            __________________
    -----------------___________________________------------------
  */

  void mod_kvm(EVMod *mod) {
    mod->data = my_calloc(sizeof(HSP_mod_KVM));
    HSP_mod_KVM *mdata = (HSP_mod_KVM *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);

    requestVNodeRole(mod, HSP_VNODE_PRIORITY_KVM);

    // open the libvirt connection - failure is not an option
    int virErr = virInitialize();
    if(virErr != 0) {
      myLog(LOG_ERR, "virInitialize() failed: %d\n", virErr);
      exit(EXIT_FAILURE);
    }

    mdata->vmsByUUID = UTHASH_NEW(HSPVMState_KVM, vm.uuid, UTHASH_DFLT);
    mdata->pollActions = UTArrayNew(UTARRAY_DFLT);

    mdata->refreshVMListSecs = sp->kvm.refreshVMListSecs ?: sp->refreshVMListSecs;
    mdata->forgetVMSecs = sp->kvm.forgetVMSecs ?: sp->forgetVMSecs;

    // register call-backs
    EVBus *pollBus = EVGetBus(mod, HSPBUS_POLL, YES);
    EVEventRx(mod, EVGetEvent(pollBus, EVEVENT_TICK), evt_tick);
    EVEventRx(mod, EVGetEvent(pollBus, EVEVENT_TOCK), evt_tock);
    EVEventRx(mod, EVGetEvent(pollBus, HSPEVENT_HOST_COUNTER_SAMPLE), evt_host_cs);
    EVEventRx(mod, EVGetEvent(pollBus, EVEVENT_FINAL), evt_final);
  }

#if defined(__cplusplus)
} /* extern "C" */
#endif
