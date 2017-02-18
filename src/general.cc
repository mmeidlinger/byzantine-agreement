#include "general.h"

namespace generals {

size_t MessagesForRound(size_t process_num, unsigned int round) {
  if (round == 0) return 1;
  return (process_num - 1 - round) * MessagesForRound(process_num, round - 1);
}

std::experimental::optional<msg::Message> ByzantineMsgFromBuf(char* buf,
                                                              size_t n) {
  // Check to make sure the size of the buffer is correct.
  if (n < sizeof(msg::ByzantineMessage)) {
    return {};
  }

  // Copy out the message part.
  msg::Message msg;
  msg::ByzantineMessage* c_msg = reinterpret_cast<msg::ByzantineMessage*>(buf);
  msg.round = ntohl(c_msg->round);
  msg.order = static_cast<msg::Order>(ntohl(c_msg->order));

  msg.ids.resize((n - sizeof(*c_msg)) / sizeof(uint32_t));
  uint32_t* id_buf = reinterpret_cast<uint32_t*>(buf + sizeof(*c_msg));
  for (size_t i = 0; i < msg.ids.size(); ++i) {
    msg.ids[i] = ntohl(id_buf[i]);
  }

  return msg;
}

std::experimental::optional<unsigned int> RoundOfAck(char* buf, size_t n) {
  // Check to make sure the size of the buffer is correct.
  if (n != sizeof(msg::Ack)) {
    return {};
  }

  msg::Ack* ack = reinterpret_cast<msg::Ack*>(buf);
  return ntohl(ack->round);
}

void SendMessage(udp::ClientPtr client, const msg::Message& msg) {
  size_t size =
      sizeof(msg::ByzantineMessage) + sizeof(uint32_t) * msg.ids.size();
  char buf[size];
  bzero(buf, size);

  // Copy the message part.
  msg::ByzantineMessage* c_msg = reinterpret_cast<msg::ByzantineMessage*>(buf);
  c_msg->type = htonl(kByzantineMessageType);
  c_msg->size = htonl(size);
  c_msg->round = htonl(msg.round);
  c_msg->order = htonl(static_cast<int>(msg.order));

  // C++ does not support flexible arrays, so we need to be a little tricky
  // here. We already made sure the buffer was the correct size by adding space
  // for each of the ids at the end of ByzantineMessage. Now we populate the
  // array.
  uint32_t* id_buf = reinterpret_cast<uint32_t*>(buf + sizeof(*c_msg));
  for (size_t i = 0; i < msg.ids.size(); ++i) {
    id_buf[i] = htonl(msg.ids[i]);
  }

  // Passed to SendWithAck to verify that any acknowledgement we hear is valid.
  auto isValidAck = [msg](udp::ClientPtr _, char* buf, size_t n) {
    auto ackRound = RoundOfAck(buf, n);
    bool valid = ackRound && *ackRound == msg.round;
    if (!valid) return udp::ServerAction::Continue;
    return udp::ServerAction::Stop;
  };

  client->SendWithAck(buf, size, kSendAttempts, isValidAck);
}

void SendAckForRound(udp::ClientPtr client, unsigned int round) {
  msg::Ack ack = {};
  ack.type = htonl(kAckType);
  ack.size = htonl(sizeof(ack));
  ack.round = htonl(round);

  char* buf = reinterpret_cast<char*>(&ack);
  client->Send(buf, sizeof(ack));
}

msg::Order Commander::Decide() {
  // Send in parallel so that some Lieutenants don't end up far ahead of others.
  ThreadGroup senders;
  msg::Message msg{round_, order_, std::vector<unsigned int>{0}};
  for (unsigned int pid = 1; pid < processes_.size(); ++pid) {
    logging::out << "Sending  " << msg << " to p" << pid << "\n";

    udp::ClientPtr client = clients_[processes_[pid]];
    senders.AddThread([client, msg] { SendMessage(client, msg); });
  }
  senders.JoinAll();
  return order_;
}

msg::Order Lieutenant::Decide() {
  server_.Listen(
      // Called on all incoming Byzantine Messages.
      [this](udp::ClientPtr client, char* buf, size_t n) {
        auto from = client->RemoteAddress();
        auto msg = ByzantineMsgFromBuf(buf, n);
        if (!msg || !ValidMessage(*msg, from)) {
          // If the message was not valid, return without trying to use it.
          return ContinueUnlessTimeout();
        }

        logging::out << "Received " << *msg << " from p" << msg->ids.back()
                     << "\n";
        SendAckForRound(client, round_);

        bool newRound = false;
        if (FirstRound()) {
          // This check shouldn't be needed.
          if (orders_seen_.size() == 0) {
            orders_seen_.insert(msg->order);
            msgs_this_round_.insert(*msg);
            newRound = true;
          }
        } else {
          if (ids_this_round_.count(msg->ids) == 0) {
            ids_this_round_.insert(msg->ids);
            msgs_this_round_.insert(*msg);
            orders_seen_.insert(msg->order);
            newRound = RoundComplete();
          }
        }

        if (newRound) {
          return MoveToNewRoundOrStop();
        }
        return ContinueUnlessTimeout();
      },
      // Called on socket timeout.
      [this]() { return HandleRoundTimeout(); });

  return DecideOrder();
}

inline msg::Order Lieutenant::DecideOrder() const {
  if (orders_seen_.size() == 1 && orders_seen_.count(msg::Order::ATTACK) == 1) {
    return msg::Order::ATTACK;
  }
  return msg::Order::RETREAT;
}

inline bool Lieutenant::RoundComplete() const {
  return ids_this_round_.size() == MessagesForRound(processes_.size(), round_);
}

udp::ServerAction Lieutenant::MoveToNewRoundOrStop() {
  if (LastRound()) {
    ClearSenders();
    return udp::ServerAction::Stop;
  } else {
    InitNewRound();
    return udp::ServerAction::Continue;
  }
}

udp::ServerAction Lieutenant::ContinueUnlessTimeout() {
  // TODO check round timeout
  return udp::ServerAction::Continue;
}

udp::ServerAction Lieutenant::HandleRoundTimeout() {
  if (FirstRound()) {
    // We can't timeout in the first round. Just continue to wait.
    return udp::ServerAction::Continue;
  }

  logging::out << "Timeout in round " << round_ << "\n";
  return MoveToNewRoundOrStop();
}

void Lieutenant::ClearSenders() {
  sender_threads_this_round_.JoinAll();
  sender_threads_this_round_.Clear();
}

void Lieutenant::InitNewRound() {
  ClearSenders();
  IncrementRound();

  std::unordered_map<unsigned int, std::vector<msg::Message>> toSend;
  for (msg::Message msg : msgs_this_round_) {
    if (msg.round != round_ - 1) {
      throw std::logic_error(
          "message in msgs_this_round_ not from current round");
    }

    msg.round = round_;
    msg.ids.push_back(id_);
    for (unsigned int pid = 0; pid < processes_.size(); ++pid) {
      // Only send to processes not already in this message.
      bool inMsg = false;
      for (auto const& id : msg.ids) {
        if (id == pid) {
          inMsg = true;
          break;
        }
      }
      if (!inMsg) {
        toSend[pid].push_back(msg);
        logging::out << "Sending  " << msg << " to p" << pid << "\n";
      }
    }
  }

  for (auto const& batch : toSend) {
    sender_threads_this_round_.AddThread([this, batch] {
      // Send each message to process serially.
      auto pid = batch.first;
      for (auto const& msg : batch.second) {
        SendMessage(clients_[processes_[pid]], msg);
      }
    });
  }

  ids_this_round_.clear();
  msgs_this_round_.clear();
}

bool Lieutenant::ValidMessage(const msg::Message& msg,
                              const net::Address& from) const {
  // Invalid if the message is from a different round.
  if (msg.round != round_) {
    return false;
  }
  // Invalid if the message has an incorrect number of ids.
  if (msg.round + 1 != msg.ids.size()) {
    return false;
  }
  // Invalid if the first message is not from the General (pid 0);
  if (msg.ids[0] != 0) {
    return false;
  }
  // Invalid if not all ids are unique.
  std::set<unsigned int> idset;
  for (auto const& id : msg.ids) {
    // Invalid if any id is out of bounds.
    if (id >= processes_.size()) {
      return false;
    }
    // Invalid if any id is our id.
    if (id == id_) {
      return false;
    }
    idset.insert(id);
  }
  if (idset.size() < msg.ids.size()) {
    return false;
  }
  // Invalid if the last id does not match the sender. This check will not
  // work for processes on the same host, because we can not know the sending
  // port of the process, only its receiving port.
  if (processes_[msg.ids.back()].hostname() != from.hostname()) {
    return false;
  }
  return true;
}

}  // namespace generals
