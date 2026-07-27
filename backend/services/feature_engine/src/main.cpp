#include "feature_engine/config.hpp"
#include "feature_engine/feature_engine.hpp"
#include "feature_engine/serialization.hpp"
#include "arrakis/runtime/metrics.hpp"
#include "arrakis/serialization/serialization.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <span>
#include <unordered_map>

namespace { volatile std::sig_atomic_t running = 1; void stop(int) { running = 0; }
std::string env(const char* name, const char* fallback) { const char* value = std::getenv(name); return value == nullptr ? fallback : value; }
bool valid_bar(const arrakis::bar_aggregator::MarketBar& bar, std::string& code) {
    if (bar.symbol.empty()) { code="empty_symbol"; return false; }
    if (bar.interval != "5m") { code="unsupported_interval"; return false; }
    if (bar.bar_start.time_since_epoch().count() <= 0 || bar.bar_end.time_since_epoch().count() <= 0 || bar.bar_end <= bar.bar_start) { code="invalid_timestamp"; return false; }
    if (!(std::isfinite(bar.open)&&std::isfinite(bar.high)&&std::isfinite(bar.low)&&std::isfinite(bar.close)) || bar.open<=0||bar.high<=0||bar.low<=0||bar.close<=0) { code="invalid_price"; return false; }
    if (!std::isfinite(bar.volume)||bar.volume<0) { code="invalid_volume"; return false; }
    if (bar.low>bar.high||bar.open<bar.low||bar.open>bar.high||bar.close<bar.low||bar.close>bar.high) { code="invalid_ohlc"; return false; }
    if (bar.event_id.empty()) { code="missing_event_id"; return false; }
    return true;
}
}

int main() {
    std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
    try {
        const auto config = arrakis::feature_engine::load_config(env("ARRAKIS_FEATURE_CONFIG", "config/feature_engine.json"));
        arrakis::feature_engine::SectorFeatureEngine engine(config.features);
        arrakis::feature_engine::TimestampAlignmentBuffer buffer(
            [&] { auto symbols=config.features.sector_symbols; symbols.insert(symbols.end(),config.features.context_symbols.begin(),config.features.context_symbols.end()); return symbols; }(), config.context_wait_timeout);
        arrakis::runtime::Metrics metrics; arrakis::runtime::MetricsServer metrics_server(metrics, config.metrics_port);
        const auto brokers=env("KAFKA_BOOTSTRAP_SERVERS","localhost:9092");
        arrakis::streaming::KafkaConsumer consumer(brokers,config.consumer_group,config.input_topic);
        arrakis::streaming::KafkaProducer producer(brokers,"feature-engine-v1");
        std::unordered_map<std::int64_t,std::vector<arrakis::streaming::KafkaRecord>> pending;
        std::unordered_map<std::string,std::int64_t> seen;
        while(running!=0){
            const auto record=consumer.poll(std::chrono::milliseconds(250));
            const auto now=std::chrono::steady_clock::now();
            for(auto& expired:buffer.expire(now)){
                const auto key=expired.event_time.time_since_epoch().count();
                auto pending_it=pending.find(key); if(pending_it!=pending.end()&&!pending_it->second.empty()){const auto error=arrakis::feature_engine::serialize_error(pending_it->second.front(),"feature-engine","incomplete_context","alignment timeout",pending_it->second.front().payload,expired.missing_symbols);producer.publish(config.error_topic,"",error);producer.flush(std::chrono::seconds(5));for(const auto& item:pending_it->second)consumer.commit(item);pending.erase(pending_it);metrics.increment("feature_events_incomplete_context_total");}
            }
            if(!record){producer.poll_events(std::chrono::milliseconds(0));continue;}
            try{
                const auto bar=arrakis::streaming::deserialize_bar(record->payload); std::string error_code;
                if(!valid_bar(bar,error_code)){const auto error=arrakis::feature_engine::serialize_error(*record,"feature-engine",error_code,"bar validation failed",record->payload);producer.publish(config.dead_letter_topic,bar.symbol,error);producer.flush(std::chrono::seconds(5));consumer.commit(*record);metrics.increment("feature_bars_rejected_total");continue;}
                const auto current_time = bar.bar_end.time_since_epoch().count();
                const auto dedup_horizon = static_cast<std::int64_t>(config.features.deduplication_window_minutes) * 60 * 1000;
                for (auto it = seen.begin(); it != seen.end();) { if (current_time > it->second && current_time - it->second > dedup_horizon) it = seen.erase(it); else ++it; }
                if(seen.contains(bar.event_id)){metrics.increment("feature_duplicate_bars_total");consumer.commit(*record);continue;}
                seen.emplace(bar.event_id,current_time); static_cast<void>(buffer.add(bar)); pending[current_time].push_back(*record); metrics.increment("feature_bars_consumed_total");
                const auto event_time=bar.bar_end;
                if(buffer.complete(event_time)){const auto bucket=buffer.take(event_time);if(bucket){const auto features=engine.add_aligned(event_time,bucket->bars_by_symbol);for(const auto& feature:features){producer.publish(config.output_topic,feature.target_symbol,arrakis::feature_engine::serialize_feature(feature));metrics.increment("feature_events_generated_total");}if(!features.empty())producer.flush(std::chrono::seconds(5));auto pending_it=pending.find(event_time.time_since_epoch().count());if(pending_it!=pending.end()){for(const auto& item:pending_it->second)consumer.commit(item);pending.erase(pending_it);}}}
                producer.poll_events(std::chrono::milliseconds(0)); metrics.set("feature_kafka_publish_failure_total",static_cast<std::int64_t>(producer.delivery_failures()));
            }catch(const std::exception& error){const auto dead=arrakis::feature_engine::serialize_error(*record,"feature-engine","malformed_bar",error.what(),record->payload);producer.publish(config.dead_letter_topic,record->key,dead);producer.flush(std::chrono::seconds(5));consumer.commit(*record);metrics.increment("feature_bars_rejected_total");std::cerr<<"{\"service\":\"feature-engine\",\"error\":\""<<error.what()<<"\"}\n";}
        }
        producer.flush(std::chrono::seconds(5)); return EXIT_SUCCESS;
    }catch(const std::exception& error){std::cerr<<"{\"service\":\"feature-engine\",\"fatal\":\""<<error.what()<<"\"}\n";return EXIT_FAILURE;}
}
