#include "transcript_manager/transcript_manager_node.hpp"

namespace whisper {
TranscriptManagerNode::TranscriptManagerNode(const rclcpp::Node::SharedPtr node_ptr)
    : node_ptr_(node_ptr), allowed_gaps(4) {

  // Subscribe to incoming token data
  auto cb_group = node_ptr_->create_callback_group(rclcpp::CallbackGroupType::Reentrant);
  rclcpp::SubscriptionOptions options;
  options.callback_group = cb_group;
  tokens_sub_ = node_ptr_->create_subscription<WhisperTokens>(
    "tokens", rclcpp::SensorDataQoS(), 
    std::bind(&TranscriptManagerNode::on_whisper_tokens_, this, std::placeholders::_1), options);

  // Action Server
  inference_action_server_ = rclcpp_action::create_server<Inference>(
    node_ptr_, "inference",
    std::bind(&TranscriptManagerNode::on_inference_, this, 
                                          std::placeholders::_1, std::placeholders::_2),
    std::bind(&TranscriptManagerNode::on_cancel_inference_, this, std::placeholders::_1),
    std::bind(&TranscriptManagerNode::on_inference_accepted_, this, std::placeholders::_1));

  // Data Initialization
  incoming_queue_ = std::make_unique<ThreadSafeRing<std::vector<Word>>>(10);

  // Outgoing data pub
  transcript_pub_ = node_ptr_->create_publisher<AudioTranscript>("transcript_stream", 10);

  clear_queue_timer_ = node_ptr_->create_wall_timer(std::chrono::milliseconds(1000), 
                std::bind(&TranscriptManagerNode::clear_queue_callback_, this));
}
void TranscriptManagerNode::clear_queue_callback_() {
  clear_queue_();
}

void TranscriptManagerNode::on_whisper_tokens_(const WhisperTokens::SharedPtr msg) {
  // print_timestamp_(ros_msg_to_chrono(msg->stamp));
  // print_msg_(msg);
  const auto &words = deserialize_msg_(msg);
  print_new_words_(words);

  incoming_queue_->enqueue(words);
  if (incoming_queue_->almost_full()) {
    auto& clk = *node_ptr_->get_clock();
    RCLCPP_WARN_THROTTLE(node_ptr_->get_logger(), clk, 5000,
                             "Transcripiton buffer full.  Dropping data.");
  }
}

rclcpp_action::GoalResponse TranscriptManagerNode::on_inference_(
                              const rclcpp_action::GoalUUID & /*uuid*/,
                             std::shared_ptr<const Inference::Goal> /*goal*/) {
  RCLCPP_INFO(node_ptr_->get_logger(), "Received inference request.");
  return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
}

rclcpp_action::CancelResponse TranscriptManagerNode::on_cancel_inference_(
            const std::shared_ptr<GoalHandleInference> /*goal_handle*/) {
  RCLCPP_INFO(node_ptr_->get_logger(), "Cancelling inference...");
  return rclcpp_action::CancelResponse::ACCEPT;
}


void TranscriptManagerNode::on_inference_accepted_(
                          const std::shared_ptr<GoalHandleInference> goal_handle) {
  RCLCPP_INFO(node_ptr_->get_logger(), "Starting inference...");
  auto feedback = std::make_shared<Inference::Feedback>();
  auto result = std::make_shared<Inference::Result>();
  inference_start_time_ = node_ptr_->now();
  auto batch_idx = 0;
  while (rclcpp::ok()) {
    if (node_ptr_->now() - inference_start_time_ > goal_handle->get_goal()->max_duration) {
      result->info = "Inference timed out.";
      RCLCPP_INFO(node_ptr_->get_logger(), result->info.c_str());
      goal_handle->succeed(result);
      return;
    }

    if (goal_handle->is_canceling()) {
      result->info = "Inference cancelled.";
      RCLCPP_INFO(node_ptr_->get_logger(), result->info.c_str());
      goal_handle->canceled(result);
      return;
    }

    // Wait for other thread
    while ( incoming_queue_->empty() ) {
      rclcpp::sleep_for(std::chrono::milliseconds(15));
    }

    // Clear queue
    std::string message;
    while ( !incoming_queue_->empty() ) {
      const auto words = incoming_queue_->dequeue();
      for (const auto &word : words) {
        if ( !word.is_segment() ) {
          message += word.get();
        }
      }
    }

    feedback->transcription = message;
    feedback->batch_idx = batch_idx;
    goal_handle->publish_feedback(feedback);
    result->transcriptions.push_back(feedback->transcription);
    RCLCPP_INFO(node_ptr_->get_logger(), "Batch %d", batch_idx);
    ++batch_idx;
  }

  if (rclcpp::ok()) {
    result->info = "Inference succeeded.";
    RCLCPP_INFO(node_ptr_->get_logger(), result->info.c_str());
    goal_handle->succeed(result);
  }
}


void TranscriptManagerNode::merge_one_(const std::vector<Word> &new_words) {
  auto stale_id = transcript_.get_stale_word_id();
  
  // 
  std::string tmp_print_str_1_;
  std::string tmp_print_str_2_;

  if (transcript_.empty()) {
    transcript_.push_back(new_words);
    RCLCPP_DEBUG(node_ptr_->get_logger(), "First Words Added");
    return;
  }

  // Get comparable strings for fuzzy lcs matching
  auto old_words = transcript_.get_words_splice();
  std::vector<std::string> comp_words_old, comp_words_new;
  std::vector<int> skipped_ids_old, skipped_ids_new;
  size_t skipped_so_far = 0;
  for (const auto &word : old_words) {
    const auto &comp_word = word.get_comparable();
    if (comp_word.empty()) {
      skipped_so_far++;
    } else {
      comp_words_old.push_back(comp_word);  
      skipped_ids_old.push_back(static_cast<int>(skipped_so_far));
      tmp_print_str_1_ += "'" + comp_words_old[comp_words_old.size() - 1] + "', ";
    }
  }
  skipped_so_far = 0;
  for (size_t i = 0; i < new_words.size(); ++i) {
    const auto &comp_word = new_words[i].get_comparable();
    if (comp_word.empty()) {
      skipped_so_far++;
    } else {
      comp_words_new.push_back(comp_word);  
      skipped_ids_new.push_back(static_cast<int>(skipped_so_far));
      tmp_print_str_2_ += "'" + comp_words_new[comp_words_new.size() - 1] + "', ";
    }
  }
  RCLCPP_DEBUG(node_ptr_->get_logger(), " ");
  RCLCPP_DEBUG(node_ptr_->get_logger(), "Comp Against:  %s", tmp_print_str_1_.c_str());
  RCLCPP_DEBUG(node_ptr_->get_logger(), "   New Words:  %s", tmp_print_str_2_.c_str());

  // Longest Common Substring with Gaps.
  //   A:  Old words (already in Transcript), B:  New words recieved from live feed
  auto [indiciesA, indiciesB] = lcs_indicies_(comp_words_old, comp_words_new, allowed_gaps);
  if (indiciesA.empty()) {
    RCLCPP_DEBUG(node_ptr_->get_logger(), "  ---No overlap");
    transcript_.push_back(new_words);
    return;
  }
  
  // Merge segments
  Transcript::Operations pending_ops;

  auto prevA = indiciesA[0], prevB = indiciesB[0];
  for(size_t i = 1; i <= indiciesA.size(); ++i) {
    // Include the offsets from skipped words
    auto prevA_id = prevA + skipped_ids_old[prevA];
    auto prevB_id = prevB + skipped_ids_new[prevB];
    RCLCPP_DEBUG(node_ptr_->get_logger(), "\tPrevA: %d,  PrevB:  %d:   %s (%f\\%d)", 
                                            prevA_id, prevB_id, 
                                            old_words[prevA_id].get().c_str(),
                                            old_words[prevA_id].get_prob(),
                                            old_words[prevA_id].get_occurrences());
    pending_ops.push_back({Transcript::OperationType::MATCHED_WORD, prevA_id, prevB_id});

    // Current index "i" may not be valid
    int curA_id = prevA_id + 1, curB_id = prevB_id + 1;
    int nextA_id, nextB_id;
    if (i == indiciesA.size()) {
      // The following merge rules will run to the end of the new_words and old_words array.
      // Most commonly, new words that do not exist in the transcript are inserted at the end.
      // TODO:  Apply these rules to the begining (before first LCS Match)
      nextA_id = old_words.size(), nextB_id = new_words.size();
    } else {
      nextA_id = indiciesA[i] + skipped_ids_old[indiciesA[i]];
      nextB_id = indiciesB[i] + skipped_ids_new[indiciesB[i]];
    }
    // RCLCPP_INFO(node_ptr_->get_logger(), "\tNextA: %d,  NextB:  %d", nextA_id, nextB_id);
    while (curA_id != nextA_id || curB_id != nextB_id) {
      // RCLCPP_INFO(node_ptr_->get_logger(), "\t\tCurA: %d,  CurB:  %d", curA_id, curB_id);
      // 
      // Custom Merge Rules
      // 
      // 0.1  If both are segments, merge transcript segment data
      if (curA_id != nextA_id && curB_id != nextB_id && 
            old_words[curA_id].is_segment() && new_words[curB_id].is_segment()) {
        {
          RCLCPP_DEBUG(node_ptr_->get_logger(), 
            "\nSegment Merge.  '\n%s'\nv.s. (new)\n%s",
                                              old_words[curA_id].get_segment_data_str().c_str(),
                                              new_words[curB_id].get_segment_data_str().c_str());
          pending_ops.push_back({Transcript::OperationType::MERGE_SEGMENTS, curA_id, curB_id});
        }
      }
      // 0.2  If the transcript has a segment not present in the update, schedule it for deletion
      else if (curA_id != nextA_id && old_words[curA_id].is_segment()) {
        pending_ops.push_back({Transcript::OperationType::DECREMENT, curA_id});
        pending_ops.push_back({Transcript::OperationType::DECREMENT, curA_id});
        curA_id++;
        continue;
      }
      // 0.3  Add segments from the update to the segment (may get deleted later)
      else if (curB_id != nextB_id && new_words[curB_id].is_segment()) {
        pending_ops.push_back({Transcript::OperationType::INSERT, curA_id, curB_id});
        curB_id++;
        continue;
      }
      // 1.  Encourage over-writing punctuation in the transcript (if the update is a word)
      if (curA_id != nextA_id && curB_id != nextB_id && 
            old_words[curA_id].is_punct() && ! new_words[curB_id].is_punct()) {
        RCLCPP_DEBUG(node_ptr_->get_logger(), 
          "\t\tWord Conflict Transcript (punct) vs update (word).  '%s' (%f\\->%d) --> '%s'",
                                            old_words[curA_id].get().c_str(),
                                            old_words[curA_id].get_prob(),
                                            old_words[curA_id].get_occurrences()-1,
                                            new_words[curB_id].get().c_str());
        pending_ops.push_back({Transcript::OperationType::DECREMENT, curA_id});
        pending_ops.push_back({Transcript::OperationType::CONFLICT, curA_id, curB_id});
        curA_id++; curB_id++;
      }
      // 1.2  Conflict when there is a gap because of missmatched words in the LCS
      else if (curA_id != nextA_id && curB_id != nextB_id) {
        RCLCPP_DEBUG(node_ptr_->get_logger(), 
                          "\t\tResolve Conflict Between '%s'(%f\\%d) and '%s'(%f\\%d)",
                                                  old_words[curA_id].get().c_str(), 
                                                  old_words[curA_id].get_prob(), 
                                                  old_words[curA_id].get_occurrences(), 
                                                  new_words[curB_id].get().c_str(), 
                                                  new_words[curB_id].get_prob(),
                                                  new_words[curB_id].get_occurrences());
        // If we have a conflict, the word's likely-hood could be decreased.
        //    - Removed:  This causes some issues with words that sound the same 
        //            i.e. are constantly in conflict.
        // pending_ops.push_back({Transcript::OperationType::DECREMENT, curA_id}); // Removed 
        pending_ops.push_back({Transcript::OperationType::CONFLICT, curA_id, curB_id});
        curA_id++; curB_id++;
      }
      // 1.3  Words appear in the audio steam (update) which are not part of the transcript
      else if (curB_id != nextB_id) {
        RCLCPP_DEBUG(node_ptr_->get_logger(), "\t\tInserting word '%s' -- Between '%s' and '%s'",
                                            new_words[curB_id].get().c_str(), 
                                            old_words[curA_id-1].get().c_str(), 
                                            curA_id == static_cast<int>(old_words.size()) ? 
                                                        "END" : old_words[curA_id].get().c_str());

        pending_ops.push_back({Transcript::OperationType::INSERT, curA_id, curB_id});
        curB_id++;
      }
      // 1__  Words in the transcript are missing from the update
      else {
        RCLCPP_DEBUG(node_ptr_->get_logger(), 
                                  "\t\tDecreasing Likelihood of word:  '%s' (%f\\%d->%d)", 
                                                old_words[curA_id].get().c_str(),
                                                old_words[curA_id].get_prob(),
                                                old_words[curA_id].get_occurrences(),
                                                old_words[curA_id].get_occurrences() - 1);
        pending_ops.push_back({Transcript::OperationType::DECREMENT, curA_id, -1});
        curA_id++;
      }
    }
    // Prep for next loop.  Move prevA and prevB to the next matching word
    prevA = indiciesA[i]; prevB = indiciesB[i];
  }

  transcript_.run(pending_ops, new_words);
  transcript_.clear_mistakes(-1);

  auto stale_id_new = std::max(stale_id, stale_id + indiciesA[0] - indiciesB[0]);
  RCLCPP_DEBUG(node_ptr_->get_logger(), "Stale id update %d -> %d", stale_id, stale_id_new );
  transcript_.set_stale_word_id(stale_id_new);
}

void TranscriptManagerNode::clear_queue_() {
  bool one_merged = false;
  while ( !incoming_queue_->empty() ) {
    one_merged = true;
    const auto words_and_segments = incoming_queue_->dequeue();
    // RCLCPP_INFO(node_ptr_->get_logger(), "Merging %ld words", words_and_segments.first.size());
    merge_one_(words_and_segments);
  }

  if (one_merged) {
    // Publish new transcript
    auto message = AudioTranscript();
    serialize_transcript_(message);
    transcript_pub_->publish(message);

    {
      // Debug Print
      const auto print_str = transcript_.get_print_str();
      RCLCPP_INFO(node_ptr_->get_logger(), "Current Transcript:   \n%s", print_str.c_str());
    }
  }
}

void TranscriptManagerNode::serialize_transcript_(AudioTranscript &msg) {
  int words_skipped = 0; // Skip adding segments into the serialized word array
  for (auto it = transcript_.begin(); it != transcript_.end(); ++it) {
    const auto & word = *it;
    if (word.is_segment()) {
      const auto &segment_data = word.get_segment_data();
      msg.seg_start_words_id.push_back(msg.words.size());
      msg.seg_start_time.push_back(chrono_to_ros_msg(segment_data->get_start()));
      msg.seg_duration_ms.push_back(segment_data->get_duration().count());
      words_skipped++;
    } else {
      msg.words.push_back(word.get());
      msg.probs.push_back(word.get_prob());
      msg.occ.push_back(word.get_occurrences());
    }
  }
  msg.active_index = transcript_.get_stale_word_id() - words_skipped;
}

void TranscriptManagerNode::print_msg_(const WhisperTokens::SharedPtr &msg) {
  std::string print_str;

  print_str += "Inference Duration:  ";
  print_str += std::to_string(msg->inference_duration);
  print_str += "\n";

  print_str += "Segment starts:  ";
  for (size_t i=0; i<msg->segment_start_token_idxs.size(); ++i) {
    print_str += std::to_string(msg->segment_start_token_idxs[i]) + ", ";
  }
  print_str += "\n";


  bool first_token = true;
  int segment_ptr = 0;
  for (size_t i=0; i<msg->token_texts.size(); ++i) {

    // If token is start of new segment
    if (static_cast<size_t>(segment_ptr) < msg->segment_start_token_idxs.size() && 
              i == static_cast<size_t>(msg->segment_start_token_idxs[segment_ptr])) {

      // First segment
      if (segment_ptr != 0) {
        print_str += "\n"; 
      }

      // Last segment
      int segment_tokens;
      if (static_cast<size_t>(segment_ptr + 1) == msg->segment_start_token_idxs.size()) {
        segment_tokens = msg->token_texts.size() - msg->segment_start_token_idxs[segment_ptr];
      } else {
        segment_tokens = msg->segment_start_token_idxs[segment_ptr + 1] - 
                                      msg->segment_start_token_idxs[segment_ptr];
      }

      // Segment data
      print_str += "Segment Tokens: ";
      print_str += std::to_string(segment_tokens);
      print_str += "  Duration: ";
      print_str += std::to_string(msg->end_times[segment_ptr] - msg->start_times[segment_ptr]);
      print_str += "  Data: ";

      first_token = true; // Dont print "|"
      ++segment_ptr;
    }

    if (!first_token) {
      print_str += "|";
    }

    print_str += msg->token_texts[i];
    first_token = false;
  }

  print_str += "\n";
  RCLCPP_INFO(node_ptr_->get_logger(), "%s", print_str.c_str());
}

void TranscriptManagerNode::print_new_words_(const std::vector<Word> &new_words) {
  std::string print_str;
  bool first_print = true;
  for (size_t i = 0; i < new_words.size();  ++i) {
    const auto &word = new_words[i];
    if (word.is_segment()) {
      const auto seg = word.get_segment_data();
      print_str += "\n";
      print_str += seg->as_str();
      first_print = true;
      continue;
    }
    if (!first_print) {
      print_str += "||";
    }
    print_str += word.get();
    first_print = false;
  }
  print_str += "\n";
  RCLCPP_INFO(node_ptr_->get_logger(), "%s", print_str.c_str());
}


void TranscriptManagerNode::print_timestamp_(std::chrono::system_clock::time_point timestamp) {
  std::time_t time_t_val = std::chrono::system_clock::to_time_t(timestamp);
  auto duration_since_epoch = timestamp.time_since_epoch();
  auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration_since_epoch);
  auto milliseconds = 
          std::chrono::duration_cast<std::chrono::milliseconds>(duration_since_epoch - seconds);
  std::tm* tm = std::localtime(&time_t_val);
  RCLCPP_INFO(node_ptr_->get_logger(), "RECIEVED:  %04d-%02d-%02d %02d:%02d:%02d.%03d\n",
         tm->tm_year + 1900,    // Year
         tm->tm_mon + 1,        // Month (0-based in tm struct)
         tm->tm_mday,           // Day
         tm->tm_hour,           // Hour
         tm->tm_min,            // Minute
         tm->tm_sec,            // Second
         static_cast<int>(milliseconds.count()));  // Milliseconds
}


 std::vector<Word> 
    TranscriptManagerNode::deserialize_msg_(const WhisperTokens::SharedPtr &msg) {
  std::vector<Word> words;
  std::vector<SingleToken> word_wip;

  auto audio_start = ros_msg_to_chrono(msg->stamp);
  
  size_t segment_ptr = 0;
  for (size_t i=0; i<msg->token_texts.size(); ++i) {
    // RCLCPP_INFO(node_ptr_->get_logger(), "i: %ld.  Token:  '%s'", i, msg->token_texts[i].c_str());

    // 
    // Deserialize Segment Data
    // 
    if (segment_ptr < msg->segment_start_token_idxs.size() &&
            i == static_cast<size_t>(msg->segment_start_token_idxs[segment_ptr])) {
      // Complete previous word before starting new segment
      if (!word_wip.empty()) {
        words.push_back({word_wip});
        word_wip.clear();
        // RCLCPP_INFO(node_ptr_->get_logger(), " NEW SEGMENT     --- Added a word");
      }

      // Get the segment end token
      size_t end_token_id;
      if (segment_ptr == msg->segment_start_token_idxs.size()-1) {
        // last segment
        end_token_id = msg->token_texts.size()-1;
      } else {
        end_token_id = static_cast<size_t>(msg->segment_start_token_idxs[segment_ptr+1] - 1);
      }
      SingleToken end_token(msg->token_texts[end_token_id], msg->token_probs[end_token_id]);


      // Create segment with:  {End token, Duration, Start timestamp}
      std::chrono::milliseconds start_ms(msg->start_times[segment_ptr]*whisper_ts_to_ms_ratio);
      std::chrono::milliseconds end_ms(msg->end_times[segment_ptr]*whisper_ts_to_ms_ratio);
      SegmentMetaData segment(end_token, end_ms - start_ms, audio_start + start_ms);
      words.push_back({segment});
      ++segment_ptr;
    }

    // 
    // Deserialize Token Data
    // 
    // Decide if we should start a new word
    if (!word_wip.empty() && !msg->token_texts[i].empty()) {
      // RCLCPP_INFO(node_ptr_->get_logger(), "   Checking for space");
      if (std::isspace(msg->token_texts[i][0])) {
        words.push_back({word_wip});
        word_wip.clear();
        // RCLCPP_INFO(node_ptr_->get_logger(), "     --- Added new word");
      }
    }

    if (is_special_token(msg->token_texts, i)) {
      // Skip whisper special tokens (e.g. [_TT_150_])
    }
    else if (my_ispunct(msg->token_texts, i)) {
      // Push back last word
      words.push_back({word_wip});
      word_wip.clear();
      // RCLCPP_INFO(node_ptr_->get_logger(), "Punct     --- Added new word");
      // Add punctuation as its own word
      words.push_back({SingleToken(msg->token_texts[i], msg->token_probs[i]), true});
    }
    else if (auto [join, num_tokens] = join_tokens(msg->token_texts, i); join) {
      std::string combined_text = combine_text(msg->token_texts, i, num_tokens);
      float combined_prob = combine_prob(msg->token_probs, i, num_tokens);
      
      // RCLCPP_INFO(node_ptr_->get_logger(), "Combining next %d tokens-> '%s'", 
      //                                                 num_tokens, combined_text.c_str());
      word_wip.push_back(SingleToken(combined_text, combined_prob));
      i += num_tokens - 1; // Skip next tokens in loop
    }
    else {
      word_wip.push_back(SingleToken(msg->token_texts[i], msg->token_probs[i]));
    }
  }

  // Final word
  if (!word_wip.empty()) {
    words.push_back({word_wip});
  }

  return words;
}


std::tuple<std::vector<int>, std::vector<int>> TranscriptManagerNode::lcs_indicies_(
                                                  const std::vector<std::string>& textA,
                                                  const std::vector<std::string>& textB,
                                                  int allowedGaps) {
  int nA = textA.size();
  int nB = textB.size();

  // 2D DP table initialized to DPEntry(0, 0)
  std::vector<std::vector<DPEntry>> dp(nA + 1, std::vector<DPEntry>(nB + 1, {0, 0}));
  std::vector<std::vector<std::pair<int, int>>> 
                      prev(nA + 1, std::vector<std::pair<int, int>>(nB + 1, {-1, -1}));

  int maxLength = 0;
  int endIndexA = -1, endIndexB = -1;

  // Fill DP table
  for (int i = 1; i <= nA; ++i) {
    // RCLCPP_INFO(node_ptr_->get_logger(), "Word i: %s", textA[i-1].c_str());
    for (int j = 1; j <= nB; ++j) {
      // RCLCPP_INFO(node_ptr_->get_logger(), "\tWord j: %s", textB[j-1].c_str());
      if (textA[i-1] == textB[j-1]) {
        // RCLCPP_INFO(node_ptr_->get_logger(), "\t\tMatch");
        dp[i][j] = {dp[i-1][j-1].length + 1, 0};
        prev[i][j] = {i-1, j-1};
      } else {
        // Case 1: skip one element from textA
        if (dp[i-1][j].gaps < allowedGaps && dp[i][j].length < dp[i-1][j].length) {  
          // RCLCPP_INFO(node_ptr_->get_logger(), "\t\t Drop word from A");
          dp[i][j] = {dp[i-1][j].length, dp[i-1][j].gaps + 1};
          prev[i][j] = prev[i-1][j];
        }

        // Case 2: skip one element from textB
        if (dp[i][j-1].gaps < allowedGaps && dp[i][j].length < dp[i][j-1].length) {
          // RCLCPP_INFO(node_ptr_->get_logger(), "\t\t Drop word from B");
          dp[i][j] = {dp[i][j-1].length, dp[i][j-1].gaps + 1};
          prev[i][j] = prev[i][j-1];
        }

        // Case 3: skip one element from textA AND textB
        if (dp[i-1][j-1].gaps < allowedGaps && dp[i][j].length < dp[i-1][j-1].length) {
          // RCLCPP_INFO(node_ptr_->get_logger(), "\t\t Drop word from A AND B");
          dp[i][j] = {dp[i-1][j-1].length, dp[i-1][j-1].gaps + 1};
          prev[i][j] = prev[i-1][j-1];
        }
      }
      // RCLCPP_INFO(node_ptr_->get_logger(), "\t\tDP[%d][%d] Length: %d  Gap: %d", 
      //             i, j, dp[i][j].length, dp[i][j].gaps);

      // Track the maximum length
      if (dp[i][j].length >= maxLength) {
        maxLength = dp[i][j].length;
        endIndexA = i;
        endIndexB = j;
        // RCLCPP_INFO(node_ptr_->get_logger(), " ---- New Best Length: %d  endpoint A: %d   endpoint B: %d", 
        //             maxLength, endIndexA, endIndexB);
      }
    }
  }

  if (maxLength == 0) {
    return {{}, {}};
  }
  
  // Backtrack to find the longest matching subsequence
  std::string print_str = "Backtrack pairs: ";
  std::vector<int> resultA, resultB;
  std::tie(endIndexA, endIndexB) = prev[endIndexA][endIndexB];
  while (endIndexA != -1 && endIndexB != -1) {
    print_str += "(" + std::to_string(endIndexA) + "," + std::to_string(endIndexB) + "), ";
    resultA.push_back(endIndexA);  
    resultB.push_back(endIndexB);
    std::tie(endIndexA, endIndexB) = prev[endIndexA][endIndexB];
  }
  // RCLCPP_INFO(node_ptr_->get_logger(), "%s", print_str.c_str());

  std::reverse(resultA.begin(), resultA.end());
  std::reverse(resultB.begin(), resultB.end());

  return {resultA, resultB};
}


} // end of namespace whisper
