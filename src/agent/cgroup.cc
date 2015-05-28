// Copyright (c) 2015, Galaxy Authors. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Author: wangtaize@baidu.com

#include "agent/cgroup.h"

#include <sstream>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>
#include <pwd.h>
#include "common/logging.h"
#include "common/util.h"
#include "common/this_thread.h"
#include "agent/downloader_manager.h"
#include "agent/resource_collector.h"
#include "agent/resource_collector_engine.h"
#include "agent/utils.h"
#include <gflags/gflags.h>

DECLARE_string(task_acct);
DECLARE_string(cgroup_root); 

namespace galaxy {

static const std::string RUNNER_META_PREFIX = "task_runner_";
static int CPU_CFS_PERIOD = 100000;
static int MIN_CPU_CFS_QUOTA = 1000;
static int CPU_SHARE_PER_CPU = 1024;
static int MIN_CPU_SHARE = 10;

int CGroupCtrl::Create(int64_t task_id, std::map<std::string, std::string>& sub_sys_map) {
    if (_support_cg.size() <= 0) {
        LOG(WARNING, "no subsystem is support");
        return -1;
    }

    std::vector<std::string>::iterator it = _support_cg.begin();

    for (; it != _support_cg.end(); ++it) {
        std::stringstream ss ;
        ss << _cg_root << "/" << *it << "/" << task_id;
        int status = mkdir(ss.str().c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

        if (status != 0) {
            if (errno == EEXIST) {
                // TODO
                LOG(WARNING, "cgroup already there");
            } else {
                LOG(FATAL, "fail to create subsystem %s ,status is %d", ss.str().c_str(), status);
                return status;
            }
        }

        sub_sys_map[*it] = ss.str();
        LOG(INFO, "create subsystem %s successfully", ss.str().c_str());
    }

    return 0;
}


//目前不支持递归删除
//删除前应该清空tasks文件
int CGroupCtrl::Destroy(int64_t task_id) {
    if (_support_cg.size() <= 0) {
        LOG(WARNING, "no subsystem is support");
        return -1;
    }

    std::vector<std::string>::iterator it = _support_cg.begin();

    for (; it != _support_cg.end(); ++it) {
        std::stringstream ss ;
        ss << _cg_root << "/" << *it << "/" << task_id;
        int status = rmdir(ss.str().c_str());
        if(status != 0 && errno != ENOENT){
            LOG(FATAL,"fail to delete subsystem %s status %d err[%d: %s]",
                    ss.str().c_str(),
                    status, 
                    errno,
                    strerror(errno));
            return status;
        }
    }

    return 0;
}

int AbstractCtrl::AttachTask(pid_t pid) {
    std::string task_file = _my_cg_root + "/" + "tasks";
    int ret = common::util::WriteIntToFile(task_file, pid);

    if (ret < 0) {
        //LOG(FATAL, "fail to attach pid  %d for %s", pid, _my_cg_root.c_str());
        return -1;
    }else{
        //LOG(INFO,"attach pid %d for %s successfully",pid, _my_cg_root.c_str());
    }

    return 0;
}

int MemoryCtrl::SetLimit(int64_t limit) {
    std::string limit_file = _my_cg_root + "/" + "memory.limit_in_bytes";
    int ret = common::util::WriteIntToFile(limit_file, limit);

    if (ret < 0) {
        //LOG(FATAL, "fail to set limt %lld for %s", limit, _my_cg_root.c_str());
        return -1;
    }

    return 0;
}

int MemoryCtrl::SetSoftLimit(int64_t soft_limit) {
    std::string soft_limit_file = _my_cg_root + "/" + "memory.soft_limit_in_bytes";
    int ret = common::util::WriteIntToFile(soft_limit_file, soft_limit);

    if (ret < 0) {
        LOG(FATAL, "fail to set soft limt %lld for %s", soft_limit, _my_cg_root.c_str());
        return -1;
    }

    return 0;
}


int CpuCtrl::SetCpuShare(int64_t cpu_share) {
    std::string cpu_share_file = _my_cg_root + "/" + "cpu.shares";
    int ret = common::util::WriteIntToFile(cpu_share_file, cpu_share);
    if (ret < 0) {
        //LOG(FATAL, "fail to set cpu share %lld for %s", cpu_share, _my_cg_root.c_str());
        return -1;
    }

    return 0;

}
int CpuCtrl::SetCpuPeriod(int64_t cpu_period) {
    std::string cpu_period_file = _my_cg_root + "/" + "cpu.cfs_period_us";
    int ret = common::util::WriteIntToFile(cpu_period_file, cpu_period);
    if (ret < 0) {
        LOG(FATAL, "fail to set cpu period %lld for %s", cpu_period, _my_cg_root.c_str());
        return -1;
    }

    return 0;

}

int CpuCtrl::SetCpuQuota(int64_t cpu_quota) {
    std::string cpu_quota_file = _my_cg_root + "/" + "cpu.cfs_quota_us";
    int ret = common::util::WriteIntToFile(cpu_quota_file, cpu_quota);
    if (ret < 0) {
        //LOG(FATAL, "fail to set cpu quota  %ld for %s", cpu_quota, _my_cg_root.c_str());
        return -1;
    }
    //LOG(INFO, "set cpu quota %ld for %s", cpu_quota, _my_cg_root.c_str());
    return 0;

}

ContainerTaskRunner::~ContainerTaskRunner() {
    if (collector_ != NULL) {
        ResourceCollectorEngine* engine
            = GetResourceCollectorEngine();
        engine->DelCollector(collector_id_);
        delete collector_;
        collector_ = NULL;
    }
    delete _cg_ctrl;
    delete _mem_ctrl;
    delete _cpu_ctrl;
    delete _cpu_acct_ctrl;
}

int ContainerTaskRunner::Prepare() {
    LOG(INFO, "prepare container for task %ld", m_task_info.task_id());
    //TODO
    std::vector<std::string> support_cg;
    support_cg.push_back("memory");
    support_cg.push_back("cpu");
    support_cg.push_back("cpuacct");
    _cg_ctrl = new CGroupCtrl(_cg_root, support_cg);
    std::map<std::string, std::string> sub_sys_map;
    int status = _cg_ctrl->Create(m_task_info.task_id(), sub_sys_map);
    // NOTE multi thread safe
    if (collector_ == NULL) {
        std::string group_path = boost::lexical_cast<std::string>(m_task_info.task_id());
        collector_ = new CGroupResourceCollector(group_path);
        ResourceCollectorEngine* engine
                = GetResourceCollectorEngine();
        collector_id_ = engine->AddCollector(collector_);
    }
    else {
        collector_->ResetCgroupName(
                boost::lexical_cast<std::string>(m_task_info.task_id()));
    }

    if (status != 0) {
        LOG(FATAL, "fail to create subsystem for task %ld,status %d", m_task_info.task_id(), status);
        return status;
    }

    _mem_ctrl = new MemoryCtrl(sub_sys_map["memory"]);
    _cpu_ctrl = new CpuCtrl(sub_sys_map["cpu"]);
    _cpu_acct_ctrl = new CpuAcctCtrl(sub_sys_map["cpuacct"]);
    int ret = Start();
    if (0 != ret) {
        return ret;
    }
    StartMonitor();
    return ret;
}

void ContainerTaskRunner::PutToCGroup(){
    int64_t mem_size = m_task_info.required_mem();
    double cpu_core = m_task_info.required_cpu();    // soft limit  cpu.share
    double cpu_limit = cpu_core;                     // hard limit  cpu.cfs_quota_us
    if (m_task_info.has_limited_cpu()) {
        cpu_limit = m_task_info.limited_cpu(); 
        if (cpu_limit < cpu_core) {
            cpu_limit = cpu_core;     
        }
    }
    assert(_mem_ctrl->SetLimit(mem_size) == 0);     

    // set cpu hard limit first
    int64_t limit = static_cast<int64_t>(cpu_limit * CPU_CFS_PERIOD);
    if (limit < MIN_CPU_CFS_QUOTA) {
        limit = MIN_CPU_CFS_QUOTA;
    }

    assert(_cpu_ctrl->SetCpuQuota(limit) == 0);
    // set cpu qutoa 
    int64_t quota = static_cast<int64_t>(cpu_core * CPU_SHARE_PER_CPU);
    if (quota < MIN_CPU_SHARE) {
        quota = MIN_CPU_SHARE; 
    }
    assert(_cpu_ctrl->SetCpuShare(quota) == 0);

    pid_t my_pid = getpid();
    assert(_mem_ctrl->AttachTask(my_pid) == 0);
    assert(_cpu_ctrl->AttachTask(my_pid) == 0);
    assert(_cpu_acct_ctrl->AttachTask(my_pid) == 0);
}

int ContainerTaskRunner::Start() {
    LOG(INFO, "start a task with id %ld", m_task_info.task_id());

    if (IsRunning() == 0) {
        LOG(WARNING, "task with id %ld has been runing", m_task_info.task_id());
        return -1;
    }

    int stdout_fd, stderr_fd;
    std::vector<int> fds;
    PrepareStart(fds, &stdout_fd, &stderr_fd);
    //sequence_id_ ++;
    passwd *pw = getpwnam(FLAGS_task_acct.c_str());
    if (NULL == pw) {
        LOG(WARNING, "getpwnam %s failed", FLAGS_task_acct.c_str());
        return -1;
    }
    uid_t userid = getuid();
    if (pw->pw_uid != userid && 0 == userid) {
        if (!file::Chown(m_workspace->GetPath(), pw->pw_uid, pw->pw_gid)) {
            LOG(WARNING, "chown %s failed", m_workspace->GetPath().c_str());
            return -1;
        }   
    }

    m_child_pid = fork();
    if (m_child_pid == 0) {
        pid_t my_pid = getpid();
        int ret = setpgid(my_pid, my_pid);
        if (ret != 0) {
            assert(0);
        }
        std::string meta_file = persistence_path_dir_ 
            + "/" + RUNNER_META_PREFIX 
            + boost::lexical_cast<std::string>(sequence_id_);
        int meta_fd = open(meta_file.c_str(), O_WRONLY | O_CREAT, S_IRWXU);
        if (meta_fd == -1) {
            assert(0);
        }
        int64_t value = my_pid;
        int len = write(meta_fd, (void*)&value, sizeof(value));
        if (len == -1) {
            close(meta_fd);
            assert(0);
        }
        value = m_task_info.task_id();
        len = write(meta_fd, (void*)&value, sizeof(value));
        if (len == -1) {
            close(meta_fd); 
            assert(0);
        }
        if (0 != fsync(meta_fd)) {
            close(meta_fd); 
            assert(0);
        }
        close(meta_fd);

        PutToCGroup();
        StartTaskAfterFork(fds, stdout_fd, stderr_fd);
    } else {
        close(stdout_fd);
        close(stderr_fd);
        if (m_child_pid == -1) {
            LOG(WARNING, "task with id %ld fork failed err[%d: %s]",
                    m_task_info.task_id(), 
                    errno,
                    strerror(errno));
            return -1; 
        }
        m_group_pid = m_child_pid;
        SetStatus(RUNNING);
    }
    return 0;
}

int ContainerTaskRunner::StartMonitor() {
    LOG(INFO, "start a task with id %ld", m_task_info.task_id());
    if (0 == m_task_info.monitor_conf().size()) {
        return -1;
    }
    if (IsProcessRunning(m_monitor_pid) == 0) {
        LOG(WARNING, "task with id %ld has been monitoring", m_task_info.task_id());
        return -1;
    }
    int stdout_fd, stderr_fd;
    std::vector<int> fds;
    PrepareStart(fds, &stdout_fd, &stderr_fd);

    std::string::size_type replace_start =
        m_task_info.monitor_conf().find("<intput>:");
    std::string::size_type replace_end =
        m_task_info.monitor_conf().find("\n", replace_start);

    char cur_path[1024] = {0};
    if (NULL == getcwd(cur_path, 1024)) {
        LOG(WARNING, "get cur path err [%d:%s]", errno, strerror(errno));
    }
    std::string log_path = std::string(cur_path) + "/"
        + m_workspace->GetPath().substr(1, m_workspace->GetPath().size() - 1)
        + m_task_info.monitor_conf().substr(replace_start + 1,
                replace_end - replace_start - 1);

    std::string new_conf = m_task_info.monitor_conf();

    new_conf.replace(replace_start + 1, replace_end - replace_start - 1, log_path);
    std::string monitor_conf = m_workspace->GetPath() + "/galaxy_monitor/monitor.conf";
    int conf_fd = open(monitor_conf.c_str(), O_WRONLY | O_CREAT, S_IRWXU);
    if (conf_fd == -1) {
        LOG(WARNING, "open monitor_conf %s failed [%d:%s]", 
                monitor_conf.c_str(),errno, strerror(errno));
    } else {
        int len = write(conf_fd, (void*)new_conf.c_str(),
                new_conf.size());
        if (len == -1) {
            LOG(WARNING, "write monitor_conf %s failed [%d:%s]",monitor_conf.c_str(),
                    errno, strerror(errno));
        }
        close(conf_fd);
    }
    m_monitor_pid = fork();
    if (m_monitor_pid == 0) {
        pid_t my_pid = getpid();
        int ret = setpgid(my_pid, my_pid);
        if (ret != 0) {
            assert(0);
        }
        PutToCGroup();
        StartMonitorAfterFork(fds, stdout_fd, stderr_fd);
    } else {
        close(stdout_fd);
        close(stderr_fd);
        if (m_monitor_pid == -1) {
            LOG(WARNING, "monitor with id %ld fork failed err[%d: %s]",
                    m_task_info.task_id(),
                    errno,
                    strerror(errno));
            return -1;
        }
        m_monitor_gid = m_monitor_pid;
    }
    return 0;
}

void ContainerTaskRunner::Status(TaskStatus* status) {
    if (collector_ != NULL) {
        status->set_cpu_usage(collector_->GetCpuUsage());
        status->set_memory_usage(collector_->GetMemoryUsage());
        LOG(WARNING, "cpu usage %f memory usage %ld",
                status->cpu_usage(), status->memory_usage());
    }
    status->set_job_id(m_task_info.job_id());
    // check if it is running
    int ret = IsRunning();
    if (ret == 0) {
        SetStatus(RUNNING);
        status->set_status(RUNNING);
    }
    else if (ret == 1) {
        SetStatus(COMPLETE);
        status->set_status(COMPLETE);
    }
    // last state is running ==> download finish
    else if (m_task_state == RUNNING
             || m_task_state == RESTART) {
        SetStatus(RESTART);
        if (ReStart() == 0) {
            status->set_status(RESTART); 
        }
        else {
            SetStatus(ERROR);
            status->set_status(ERROR); 
        }
    }
    // other state
    else {
        status->set_status(m_task_state); 
    }
    LOG(WARNING, "task with id %ld state %s", 
            m_task_info.task_id(), 
            TaskState_Name(TaskState(m_task_state)).c_str());
    return;
}

void ContainerTaskRunner::StopPost() {
    if (collector_ != NULL) {
        collector_->Clear();
    }
    std::string meta_file = persistence_path_dir_ 
        + "/" + RUNNER_META_PREFIX 
        + boost::lexical_cast<std::string>(sequence_id_);
    if (!file::Remove(meta_file)) {
        LOG(WARNING, "remove %s failed", meta_file.c_str()); 
    }
    std::string monitor_conf = m_workspace->GetPath()
        + "/galaxy_monitor/monitor_conf";
    if (!file::Remove(monitor_conf)) {
        LOG(WARNING, "rm monitor cfg failed rm %s",
                monitor_conf.c_str());
    }
    return;
}

int ContainerTaskRunner::Stop(){
    int status = AbstractTaskRunner::Stop();
    LOG(INFO,"stop  task %ld  with status %d",m_task_info.task_id(),status);
    if(status != 0 ){
        return status;
    }
    if (_cg_ctrl != NULL) {
        //sleep 500 ms for cgroup clear tasks
        common::ThisThread::Sleep(500);
        status = _cg_ctrl->Destroy(m_task_info.task_id());
        LOG(INFO,"destroy cgroup for task %ld with status %d",m_task_info.task_id(),status);
    }
    return status;
}

bool ContainerTaskRunner::RecoverRunner(const std::string& persistence_path) {
    std::vector<std::string> files;
    if (!file::GetDirFilesByPrefix(
                persistence_path,
                RUNNER_META_PREFIX,
                &files)) {
        LOG(WARNING, "get meta files failed"); 
        return false;
    }

    if (files.size() == 0) {
        return true; 
    }

    int max_seq_id = -1;
    std::string last_meta_file;
    for (size_t i = 0; i < files.size(); i++) {
        std::string file = files[i]; 
        size_t pos = file.find(RUNNER_META_PREFIX);
        if (pos == std::string::npos) {
            continue; 
        }

        if (pos + RUNNER_META_PREFIX.size() >= file.size()) {
            LOG(WARNING, "meta file format err %s", file.c_str()); 
            continue;
        }

        int cur_id = atoi(file.substr(
                    pos + RUNNER_META_PREFIX.size()).c_str());
        if (max_seq_id < cur_id) {
            max_seq_id = cur_id; 
            last_meta_file = file;
        }
    }

    if (max_seq_id < 0) {
        return false; 
    }

    std::string meta_file = last_meta_file;
    LOG(DEBUG, "start to recover %s", meta_file.c_str());
    int fin = open(meta_file.c_str(), O_RDONLY);
    if (fin == -1) {
        LOG(WARNING, "open meta file failed %s err[%d: %s]", 
                meta_file.c_str(),
                errno,
                strerror(errno)); 
        return false;
    }

    size_t value;
    int len = read(fin, (void*)&value, sizeof(value));
    if (len == -1) {
        LOG(WARNING, "read meta file failed err[%d: %s]",
                errno,
                strerror(errno)); 
        close(fin);
        return false;
    }
    LOG(DEBUG, "recove gpid %lu", value);
    int ret = killpg((pid_t)value, 9);
    if (ret != 0 && errno != ESRCH) {
        LOG(WARNING, "fail to kill process group %lu", value); 
        return false;
    }

    value = 0;
    len = read(fin, (void*)&value, sizeof(value));
    if (len == -1) {
        LOG(WARNING, "read meta file failed err[%d: %s]",
                errno,
                strerror(errno));
        close(fin);
        return false;
    }
    close(fin);

    std::vector<std::string> support_cgroup;  
    // TODO configable
    support_cgroup.push_back("memory");
    support_cgroup.push_back("cpu");
    support_cgroup.push_back("cpuacct");
    
    LOG(DEBUG, "destroy cgroup %lu", value);
    CGroupCtrl ctl(FLAGS_cgroup_root, support_cgroup);
    int max_retry_times = 10;
    ret = -1;
    while (max_retry_times-- > 0) {
        ret = ctl.Destroy(value);
        if (ret != 0) {
            common::ThisThread::Sleep(100);
            continue;
        }    
        break;
    }
    if (ret == 0) {
        LOG(DEBUG, "destroy cgroup %lu success", value);
        return true; 
    }
    return false;
}

}
