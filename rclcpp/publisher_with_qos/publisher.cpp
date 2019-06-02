// Copyright 2019 Amazon.com, Inc. or its affiliates. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

#include <termios.h>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"

using namespace std::chrono_literals;

//
// helper functions
//
static struct termios old_termios;
static struct termios new_termios;

/* Initialize new terminal i/o settings */
void initTermios(int echo) 
{
  tcgetattr(0, &old_termios); /* grab old terminal i/o settings */
  new_termios = old_termios; /* make new settings same as old settings */
  new_termios.c_lflag &= ~ICANON; /* disable buffered i/o */
  if (echo) {
      new_termios.c_lflag |= ECHO; /* set echo mode */
  } else {
      new_termios.c_lflag &= ~ECHO; /* set no echo mode */
  }
  tcsetattr(0, TCSANOW, &new_termios); /* use these new terminal i/o settings now */
}

/* Restore old terminal i/o settings */
void resetTermios(void) 
{
  tcsetattr(0, TCSANOW, &old_termios);
}

/* Read 1 character - echo defines echo mode */
char getch_(int echo) 
{
  char ch;
  initTermios(echo);
  ch = getchar();
  resetTermios();
  return ch;
}

/* Read 1 character without echo */
char getch(void) 
{
  return getch_(0);
}

/* Read 1 character with echo */
char getche(void) 
{
  return getch_(1);
}

double rmw_time_to_seconds(const rmw_time_t & time)
{
  double result = time.sec;
  result += 1e-9 * time.nsec;
  return result;
}

class CommandPrompt
{
public:
  bool is_active() const
  {
    return run_.load(std::memory_order_relaxed);
  }

  void operator()()
  {
    while (run_.load(std::memory_order_relaxed)) {
      char cmd = getch();
      handle_cmd(cmd);
    }
  }

  virtual void handle_cmd(const char cmd) const = 0;

  void start()
  {
    thread_ = std::thread(std::ref(*this));
    run_.store(true, std::memory_order_relaxed);
  }

  void stop()
  {
    run_.store(false, std::memory_order_relaxed);
    thread_.join();
  }

private:
  std::thread thread_;
  std::atomic<bool> run_;
};

class PublisherWithQOS : public rclcpp::Node
{
public:
  PublisherWithQOS() : Node("publisher"), count_(0)
  {
    publisher_ = this->create_publisher<std_msgs::msg::String>("topic", 10);
    print_qos();
    toggle_send();
  }

  bool assert_node_liveliness() const
  {
    std::cout << "asserting node liveliness" << std::endl;
    return assert_liveliness();
  }

  bool assert_publisher_liveliness() const
  {
    std::cout << "asserting publisher liveliness" << std::endl;
    return publisher_->assert_liveliness();
  }

  void toggle_send()
  {
    if (timer_ == nullptr) {
      std::cout << "start sending messages" << std::endl;
      timer_ = this->create_wall_timer(500ms, [this] { timer_callback(); });
    } else {
      std::cout << "stop sending messages" << std::endl;
      timer_->cancel();
      timer_ = nullptr;
    }
  }

  void print_qos() const
  {
    rmw_qos_profile_t qos = publisher_->get_actual_qos();

    std::cout << "HISTORY POLICY: ";
    switch (qos.history) {
      case RMW_QOS_POLICY_HISTORY_KEEP_LAST:
        std::cout << "keep last";
        break;
      case RMW_QOS_POLICY_HISTORY_KEEP_ALL:
        std::cout << "keep all";
        break;
      default:
        std::cout << "invalid";
    }
    std::cout << " (depth: " << qos.depth << ')' << std::endl;

    std::cout << "RELIABILITY POLICY: ";
    switch (qos.reliability) {
      case RMW_QOS_POLICY_RELIABILITY_RELIABLE:
        std::cout << "reliable";
        break;
      case RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT:
        std::cout << "best effort";
        break;
      default:
        std::cout << "invalid";
    }
    std::cout << std::endl;

    std::cout << "DURABILITY POLICY: ";
    switch (qos.durability) {
      case RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL:
        std::cout << "transient local";
        break;
      case RMW_QOS_POLICY_DURABILITY_VOLATILE:
        std::cout << "volatile";
        break;
      default:
        std::cout << "invalid";
    }
    std::cout << std::endl;

    std::cout << "DEADLINE: " << rmw_time_to_seconds(qos.deadline) << std::endl;

    std::cout << "LIFESPAN: " << rmw_time_to_seconds(qos.lifespan) << std::endl;

    std::cout << "LIVELINESS POLICY: ";
    switch (qos.liveliness) {
      case RMW_QOS_POLICY_LIVELINESS_AUTOMATIC:
        std::cout << "automatic";
        break;
      case RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_NODE:
        std::cout << "manual by node";
        break;
      case RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC:
        std::cout << "manual by topic";
        break;
      default:
        std::cout << "invalid";
    }
    std::cout << " (lease duration: " << rmw_time_to_seconds(qos.liveliness_lease_duration) << ')'
              << std::endl;
  }

private:
  void timer_callback()
  {
    auto message = std_msgs::msg::String();
    message.data = "Hello, world! " + std::to_string(count_++);
    RCLCPP_INFO(this->get_logger(), "Publishing: '%s'", message.data.c_str());
    publisher_->publish(message);
  }

  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr publisher_;
  rclcpp::TimerBase::SharedPtr timer_;
  size_t count_;
};

class CommandHandler : public CommandPrompt
{
public:
  CommandHandler(PublisherWithQOS * publisher) : publisher_(publisher) {}

  virtual void handle_cmd(const char cmd) const override
  {
    // std::cout << "handle_cmd " << cmd << std::endl;
    if (cmd == 'n') {
      // manually assert liveliness of node
      publisher_->assert_node_liveliness();
    } else if (cmd == 'p') {
      // manually assert liveliness of publisher
      publisher_->assert_publisher_liveliness();
    } else if (cmd == 's') {
      // toggle publishing of messages
      publisher_->toggle_send();
    } else if (cmd == 'q') {
      // print the qos settings
      publisher_->print_qos();
    }
  }

private:
  PublisherWithQOS * publisher_;
};

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);

  auto node = std::make_shared<PublisherWithQOS>();
  CommandHandler cmd_handler(node.get());

  cmd_handler.start();
  rclcpp::spin(node);
  cmd_handler.stop();

  rclcpp::shutdown();
  return 0;
}
