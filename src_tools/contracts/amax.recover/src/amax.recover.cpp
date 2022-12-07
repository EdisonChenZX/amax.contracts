#include <amax.recover/amax.recover.hpp>

#include<math.hpp>

#include <utils.hpp>
#include <amax_proxy.hpp>

static constexpr eosio::name active_permission{"active"_n};
static constexpr symbol   APL_SYMBOL          = symbol(symbol_code("APL"), 4);
static constexpr eosio::name MT_BANK{"amax.token"_n};

#define ALLOT_APPLE(farm_contract, lease_id, to, quantity, memo) \
    {   aplink::farm::allot_action(farm_contract, { {_self, active_perm} }).send( \
            lease_id, to, quantity, memo );}

namespace amax {

using namespace std;

#define CHECKC(exp, code, msg) \
   { if (!(exp)) eosio::check(false, string("[[") + to_string((int)code) + string("]] ") + msg); }


   inline int64_t get_precision(const symbol &s) {
      int64_t digit = s.precision();
      CHECK(digit >= 0 && digit <= 18, "precision digit " + std::to_string(digit) + " should be in range[0,18]");
      return calc_precision(digit);
   }

   void amax_recover::init( const uint8_t& recover_threshold, 
                           const name amax_proxy_contract) {
      require_auth( _self );
      _gstate.recover_threshold                 = recover_threshold;
      _gstate.amax_proxy_contract         = amax_proxy_contract;
   }

   void amax_recover::bindaccount( const name& account, const name& default_checker ) {
      require_auth ( _gstate.amax_proxy_contract );
      check(is_account(account), "account invalid: " + account.to_string());
      recover_auth_t::idx accountaudits(_self, _self.value);
      auto audit_ptr     = accountaudits.find(account.value);
      CHECKC( audit_ptr == accountaudits.end(), err::RECORD_EXISTING, "account already exist. ");
      auto now           = current_time_point();

      uint8_t score = 0;
      bool required = _audit_item(default_checker, score);

      accountaudits.emplace( _self, [&]( auto& row ) {
         row.checker_requirements[default_checker]   = required ? RecoverConditionStatus::REQUIRED : RecoverConditionStatus::OPTIONAL;
         row.account 		                     = account;
         row.threshold                          = score;
         row.created_at                         = now;
         row.updated_at                         = now;
      });   
   }

   void amax_recover::addauth( const name& account, const name& contract ) {
      CHECKC( has_auth(account) , err::NO_AUTH, "no auth for operate" )
      uint8_t score = 0;
      _audit_item(contract, score);

      recover_auth_t::idx account_audits(_self, _self.value);
      auto audit_ptr     = account_audits.find(account.value);
      CHECKC( audit_ptr != account_audits.end(), err::RECORD_NOT_FOUND, "account not exist. ");
      auto now           = current_time_point();

      CHECKC(audit_ptr->checker_requirements.count(contract) == 0, err::RECORD_EXISTING, "contract already existed") 

      account_audits.modify( *audit_ptr, _self, [&]( auto& row ) {
         row.checker_requirements[contract]  = RecoverConditionStatus::REGISTED;
         row.created_at                 = now;
         row.updated_at                 = now;
      });   
   }

   void amax_recover::checkauth( const name& checker_contract, const name& account ) {
      require_auth ( checker_contract ); 
      uint8_t score         = 0;
      bool required         = _audit_item(checker_contract, score);

      recover_auth_t::idx account_audits(_self, _self.value);
      auto audit_ptr        = account_audits.find(account.value);
      CHECKC( audit_ptr    != account_audits.end(), err::RECORD_NOT_FOUND, "account record not exist: " + account.to_string());
      CHECKC( audit_ptr->checker_requirements.count(checker_contract) != 0, err::RECORD_NOT_FOUND, "contract not found:" +checker_contract.to_string()  )

      account_audits.modify(*audit_ptr, _self, [&]( auto& row ) {
         row.checker_requirements[checker_contract]  = required ? RecoverConditionStatus::REQUIRED : RecoverConditionStatus::OPTIONAL;
         if( audit_ptr->threshold < _gstate.recover_threshold ) row.threshold = _gstate.recover_threshold;
         row.created_at                 = current_time_point();
      });
   }

   void amax_recover::createorder(
                        const uint64_t&            sn,
                        const name&                checker_contract,
                        const name&                account,
                        const bool&                manual_check_required,
                        const uint8_t&             score,
                        const recover_target_type& recover_target) {

      require_auth(checker_contract);
      
      uint8_t  answer_score_limit  = 0;
      _audit_item(checker_contract, answer_score_limit);

      CHECKC(score <= answer_score_limit, err::PARAM_ERROR, "score peram error")

      recover_auth_t::idx accountaudits(_self, _self.value);
      auto audit_ptr     = accountaudits.find(account.value);
      CHECKC( audit_ptr != accountaudits.end(), err::RECORD_NOT_FOUND, "account not exist. ");
      map<name, uint8_t> scores;
      for ( auto& [key, value]: audit_ptr->checker_requirements ) {
         if (value == RecoverConditionStatus::REQUIRED) {
            scores[key] = 0;
         }
      }
   
      auto duration_second    = order_expiry_duration;
      if (manual_check_required) {
         audit_conf_t::idx_t auditscores(_self, _self.value);
         auto auditscore_idx = auditscores.get_index<"audittype"_n>();
         auto auditscore_itr =  auditscore_idx.find(CheckType::MANUAL.value);
         CHECKC( auditscore_itr != auditscore_idx.end(), err::RECORD_NOT_FOUND, "record not existed, " + CheckType::MANUAL.to_string());

         duration_second      = manual_order_expiry_duration;
         scores[auditscore_itr->contract] = 0;
      }

      scores[checker_contract] = score;
      
      recover_order_t::idx_t orders( _self, _self.value );
      auto account_index 			      = orders.get_index<"accountidx"_n>();
      auto order_itr 			         = account_index.find( account.value );
      CHECKC( order_itr == account_index.end(), err::RECORD_EXISTING, "order already existed. ");

      _gstate.last_order_id ++;
      auto order_id           = _gstate.last_order_id; 
      auto now                = current_time_point();

      orders.emplace( _self, [&]( auto& row ) {
         row.id 					      = order_id;
         row.sn             = sn;
         row.account 			      = account;
         row.scores                 = scores;
         row.recover_type           = UpdateActionType::PUBKEY;
         row.recover_target         = recover_target;
         row.pay_status             = PayStatus::NOPAY;
         row.created_at             = now;
         row.expired_at             = now + eosio::seconds(duration_second);
         row.status                 = OrderStatus::PENDING;
      });
   
   }


   void amax_recover::setscore( const name& checker_contract, const name& account, const uint64_t& order_id, const uint8_t& score) {
      require_auth(checker_contract);

      recover_auth_t::idx accountaudits(_self, _self.value);
      auto audit_ptr     = accountaudits.find(account.value);
      CHECKC( audit_ptr != accountaudits.end(), err::RECORD_NOT_FOUND, "account not exist. ");
      CHECKC(audit_ptr->checker_requirements.count(checker_contract) > 0 , err::NO_AUTH, "no auth for set score: " + account.to_string());

      uint8_t  answer_score_limit  = 0;
      _audit_item(checker_contract, answer_score_limit);
      
      recover_order_t::idx_t orders(_self, _self.value);
      auto order_ptr     = orders.find(order_id);
      CHECKC( order_ptr != orders.end(), err::RECORD_NOT_FOUND, "order not found. ");
      CHECKC(answer_score_limit >= score && score > 0, err::PARAM_ERROR, "scores must between 0 and " + to_string(score))
      CHECKC(order_ptr->account == account , err::PARAM_ERROR, "account error: "+ account.to_string() )

      CHECKC(order_ptr->expired_at > current_time_point(), err::TIME_EXPIRED, "order already time expired")

      orders.modify(*order_ptr, _self, [&]( auto& row ) {
         row.scores[checker_contract]    = score;
         row.updated_at                = current_time_point();
      });
   }

   void amax_recover::closeorder( const name& submitter, const uint64_t& order_id) {
      CHECKC( has_auth(submitter) , err::NO_AUTH, "amax_recover no auth for operate" )

      recover_order_t::idx_t orders(_self, _self.value);
      auto order_ptr     = orders.find(order_id);
      CHECKC( order_ptr != orders.end(), err::RECORD_NOT_FOUND, "order not found. "); 
      CHECKC(order_ptr->expired_at > current_time_point(), err::TIME_EXPIRED, "order already time expired")

      audit_conf_t::idx_t auditscores(_self, _self.value);
      auto auditscore_idx = auditscores.get_index<"audittype"_n>();
      auto auditscore_itr =  auditscore_idx.find(CheckType::MANUAL.value);
   

      auto total_score = 0;
      for (auto& [key, value]: order_ptr->scores) {
         CHECKC(value != 0 , err::NEED_REQUIRED_CHECK, "required check: " + key.to_string())
         if( auditscore_itr == auditscore_idx.end() || auditscore_itr->contract != key ) {
            total_score += value; 
         }
      }

      recover_auth_t::idx accountaudits(_self, _self.value);
      auto audit_ptr     = accountaudits.find(order_ptr->account.value);
      CHECKC( audit_ptr != accountaudits.end(), err::RECORD_NOT_FOUND, "order not exist. ");
      CHECKC( total_score >= audit_ptr->threshold, err::SCORE_NOT_ENOUGH, "score not enough" );

      accountaudits.modify( *audit_ptr, _self, [&]( auto& row ) {
         row.recovered_at  = current_time_point();
      });

      _update_auth(order_ptr->account, std::get<eosio::public_key>(order_ptr->recover_target));
      orders.erase(order_ptr);
   }

   void amax_recover::delorder( const name& submitter, const uint64_t& order_id) {
      CHECKC( has_auth(submitter) , err::NO_AUTH, "no auth for operate" )

      recover_order_t::idx_t orders(_self, _self.value);
      auto order_ptr     = orders.find(order_id);
      CHECKC( order_ptr != orders.end(), err::RECORD_NOT_FOUND, "order not found. "); 

      // CHECKC(order_ptr->expired_at < current_time_point(), err::STATUS_ERROR, "order has not expired")
      orders.erase(order_ptr);
   }

   void amax_recover::addauditconf( const name& check_contract, const name& audit_type, const audit_conf_s& conf ) {
      CHECKC(has_auth(_self),  err::NO_AUTH, "no auth for operate"); 

      CHECKC(  audit_type == CheckType::MOBILENO || 
               audit_type == CheckType::ANSWER ||
               audit_type == CheckType::DID ||
               audit_type == CheckType::TELEGRAM ||
               audit_type == CheckType::FACEBOOK ||
               audit_type == CheckType::MANUAL , err::PARAM_ERROR, "audit type error: " + audit_type.to_string())

      CHECKC( conf.status == ContractStatus::RUNNING || conf.status == ContractStatus::STOPPED, 
                     err::PARAM_ERROR, "contract status error " + conf.status.to_string() )

      CHECKC( conf.max_score > 0 , err::PARAM_ERROR, "score error ")

      check(is_account(check_contract), "check_contract invalid: " + check_contract.to_string());

      audit_conf_t::idx_t auditscores(_self, _self.value);
      auto auditscore_ptr     = auditscores.find(check_contract.value);
      if( auditscore_ptr != auditscores.end() ) {
         auditscores.modify(*auditscore_ptr, _self, [&]( auto& row ) {
            row.audit_type    = audit_type;
            row.charge        = conf.charge;
            if( conf.title.length() > 0 )       row.title         = conf.title;
            if( conf.desc.length() > 0 )        row.desc  = conf.desc;
            if( conf.max_score > 0 )   row.max_score   = conf.max_score;
            row.check_required = conf.check_required;
            row.status        = conf.status;
         });   
      } else {
         auditscores.emplace(_self, [&]( auto& row ) {
            row.contract      = check_contract;
            row.audit_type    = audit_type;
            row.charge        = conf.charge;
            row.title         = conf.title;
            row.desc          = conf.desc;
            row.url           = conf.url;
            row.charge        = conf.charge;
            row.max_score     = conf.max_score;
            row.check_required = conf.check_required;
            row.status        = conf.status;
         });
      }
   }

   void amax_recover::delauditconf(  const name& contract_name ) {
      CHECKC(has_auth(_self),  err::NO_AUTH, "no auth for operate");      

      audit_conf_t::idx_t auditscores(_self, _self.value);
      auto auditscore_ptr     = auditscores.find(contract_name.value);

      CHECKC( auditscore_ptr != auditscores.end(), err::RECORD_NOT_FOUND, "auditscore not exist. ");
      auditscores.erase(auditscore_ptr);
   }

   bool amax_recover::_audit_item(const name& contract, uint8_t& score) {
      audit_conf_t::idx_t auditscores(_self, _self.value);

      auto auditscore_ptr     = auditscores.find(contract.value);
      CHECKC( auditscore_ptr != auditscores.end(), err::RECORD_NOT_FOUND, "audit_conf_t contract not exist:  " + contract.to_string());
      CHECKC( auditscore_ptr->status == ContractStatus::RUNNING, err::STATUS_ERROR, "contract status is error: " + contract.to_string());
      score = auditscore_ptr->max_score;

      return auditscore_ptr->check_required;
   }

   void amax_recover::_update_auth( const name& account, const eosio::public_key& pubkey ) {
      amax_proxy::updateauth_action updateauth_act(_gstate.amax_proxy_contract, { {get_self(), "active"_n} });
      updateauth_act.send( account, pubkey);   
   }

}//namespace amax