// Baidu RPC - A framework to host and access services throughout Baidu.
// Copyright (c) 2014 Baidu.com, Inc. All Rights Reserved
//
// If you have any problem, contact us:
//   Baidu Hi : group 1296497 
//   Email    : pbrpc@baidu.com
//   Wiki     : http://wiki.baidu.com/display/RPC/baidu-rpc

// Benchmark http-server by multiple threads.

#include <gflags/gflags.h>
#include <bthread/bthread.h>
#include <base/logging.h>
#include <base/string_printf.h>
#include <base/time.h>
#include <base/macros.h>
#include <brpc/channel.h>
#include <brpc/server.h>
#include <bvar/bvar.h>

DEFINE_string(data, "", "POST this data to the http server");
DEFINE_int32(thread_num, 50, "Number of threads to send requests");
DEFINE_bool(use_bthread, false, "Use bthread to send requests");
DEFINE_string(connection_type, "", "Connection type. Available values: single, pooled, short");
DEFINE_string(url, "0.0.0.0:8010/HttpService/Echo", "url of server");
DEFINE_string(load_balancer, "", "The algorithm for load balancing");
DEFINE_int32(timeout_ms, 100, "RPC timeout in milliseconds");
DEFINE_int32(max_retry, 3, "Max retries(not including the first RPC)"); 
DEFINE_bool(dont_fail, false, "Print fatal when some call failed");
DEFINE_int32(dummy_port, 0, "Launch dummy server at this port");

bvar::LatencyRecorder g_latency_recorder("client");

static void* sender(void* arg) {
    brpc::Channel* channel = static_cast<brpc::Channel*>(arg);

    while (!brpc::IsAskedToQuit()) {
        // We will receive response synchronously, safe to put variables
        // on stack.
        brpc::Controller cntl;

        cntl.set_timeout_ms(FLAGS_timeout_ms/*milliseconds*/);
        cntl.set_max_retry(FLAGS_max_retry);
        cntl.http_request().uri() = FLAGS_url;
        if (!FLAGS_data.empty()) {
            cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
            cntl.request_attachment().append(FLAGS_data);
        }

        // Because `done'(last parameter) is NULL, this function waits until
        // the response comes back or error occurs(including timedout).
        channel->CallMethod(NULL, &cntl, NULL, NULL, NULL);
        if (!cntl.Failed()) {
            g_latency_recorder << cntl.latency_us();
        } else {
            CHECK(brpc::IsAskedToQuit() || !FLAGS_dont_fail)
                << "error=" << cntl.ErrorText() << " latency=" << cntl.latency_us();
            // We can't connect to the server, sleep a while. Notice that this
            // is a specific sleeping to prevent this thread from spinning too
            // fast. You should continue the business logic in a production 
            // server rather than sleeping.
            bthread_usleep(50000);
        }
    }
    return NULL;
}

int main(int argc, char* argv[]) {
    // Parse gflags. We recommend you to use gflags as well.
    google::ParseCommandLineFlags(&argc, &argv, true);

    // A Channel represents a communication line to a Server. Notice that 
    // Channel is thread-safe and can be shared by all threads in your program.
    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.protocol = "http";
    options.connection_type = FLAGS_connection_type;
    
    // Initialize the channel, NULL means using default options. 
    // options, see `brpc/channel.h'.
    if (channel.Init(FLAGS_url.c_str(), FLAGS_load_balancer.c_str(), &options) != 0) {
        LOG(ERROR) << "Fail to initialize channel";
        return -1;
    }

    std::vector<bthread_t> tids;
    tids.resize(FLAGS_thread_num);
    if (!FLAGS_use_bthread) {
        for (int i = 0; i < FLAGS_thread_num; ++i) {
            if (pthread_create(&tids[i], NULL, sender, &channel) != 0) {
                LOG(ERROR) << "Fail to create pthread";
                return -1;
            }
        }
    } else {
        for (int i = 0; i < FLAGS_thread_num; ++i) {
            if (bthread_start_background(
                    &tids[i], NULL, sender, &channel) != 0) {
                LOG(ERROR) << "Fail to create bthread";
                return -1;
            }
        }
    }

    if (FLAGS_dummy_port > 0) {
        brpc::StartDummyServerAt(FLAGS_dummy_port);
    }

    while (!brpc::IsAskedToQuit()) {
        sleep(1);
        LOG(INFO) << "Sending http request at qps=" << g_latency_recorder.qps(1)
                  << " latency=" << g_latency_recorder.latency(1);
    }

    LOG(INFO) << "benchmark_http is going to quit";
    for (int i = 0; i < FLAGS_thread_num; ++i) {
        if (!FLAGS_use_bthread) {
            pthread_join(tids[i], NULL);
        } else {
            bthread_join(tids[i], NULL);
        }
    }

    return 0;
}