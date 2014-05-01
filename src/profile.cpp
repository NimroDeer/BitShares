#include <bts/profile.hpp>
#include <bts/keychain.hpp>
#include <bts/addressbook/addressbook.hpp>
#include <bts/bitname/bitname_hash.hpp>
#include <bts/addressbook/contact.hpp>
#include <bts/db/level_map.hpp>
#include <bts/db/level_pod_map.hpp>
#include <fc/io/raw.hpp>
#include <fc/io/raw_variant.hpp>
#include <fc/interprocess/mmap_struct.hpp>

#include <fc/crypto/aes.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/exception/exception.hpp>
#include <fc/io/fstream.hpp>
#include <fc/filesystem.hpp>
#include <iostream>

namespace bts { namespace addressbook {
REGISTER_DB_OBJECT(wallet_identity,0)
} }

#define KEYHOTEE_MASTER_KEY_FILE ".keyhotee_master.key"

namespace bts {

  namespace detail 
  {
     class profile_impl
     {
        public:
            keychain                                        _keychain;
            addressbook::addressbook_ptr                    _addressbook;
            bitchat::message_db_ptr                         _inbox_db;
            bitchat::message_db_ptr                         _draft_db;
            bitchat::message_db_ptr                         _pending_db;
            bitchat::message_db_ptr                         _sent_db;
            bitchat::message_db_ptr                         _spam_db;
            bitchat::message_db_ptr                         _chat_db;
            // current authorization requests - requiring user action (accept, reject, block),
            //    not handled queries are stored and loaded every time when start the application
            //  so there is no need to search the entire database _auth_db
            bitchat::message_db_ptr                         _request_db;
            // database of all served requests of authorization
            bitchat::message_db_ptr                         _auth_db;
            db::level_map<std::string, addressbook::wallet_identity>            _idents;
            std::wstring                                    _profile_name;
            
            fc::mmap_struct<fc::time_point>                 _last_sync_time;
/*
            void import_draft( const std::vector<char> crypt, const fc::uint512& key )
            {
                auto plain = fc::aes_decrypt( key, crypt );
                _drafts.push_back( fc::raw::unpack<bts::bitchat::email_draft>(plain) );
            }
*/
     };

  } // namespace detail

  profile::profile()
  :my( new detail::profile_impl() )
  {
    my->_addressbook = std::make_shared<addressbook::addressbook>();
 
    my->_inbox_db  = std::make_shared<bitchat::message_db>();
    my->_draft_db  = std::make_shared<bitchat::message_db>();
    my->_pending_db  = std::make_shared<bitchat::message_db>();
    my->_sent_db  = std::make_shared<bitchat::message_db>();
    my->_spam_db = std::make_shared<bitchat::message_db>();
    my->_chat_db = std::make_shared<bitchat::message_db>();
    my->_request_db= std::make_shared<bitchat::message_db>();
    my->_auth_db = std::make_shared<bitchat::message_db>();
  }
  

  profile::~profile()
  {}

  fc::time_point       profile::get_last_sync_time()const
  {
      return *my->_last_sync_time;
  }
  void     profile::set_last_sync_time( const fc::time_point& n )
  {
      ilog("time=${t}",("t",n));
      *my->_last_sync_time = n;
  }

  void profile::create( const fc::path& profile_dir, const profile_config& cfg, const std::string& password,
    std::function<void(double)> progress )
  { try {
       fc::sha512::encoder encoder;
       fc::raw::pack( encoder, cfg );
       auto seed = encoder.result();

       /// note: this could take a minute
       std::cerr << "start keychain::stretch_seed\n";
       auto stretched_seed   = keychain::stretch_seed( seed, progress );
       std::cerr << "finished stretch_seed\n";
       ilog("finished stretch_seed");
      // FC_ASSERT( !fc::exists( profile_dir ) );
       std::cerr << "another create_directories\n";
       fc::create_directories( profile_dir );
       
       auto profile_cfg_key  = fc::sha512::hash( password.c_str(), password.size() );
       std::cerr << "encrypt and save encrypted seed\n";
       fc::aes_save( profile_dir / KEYHOTEE_MASTER_KEY_FILE, profile_cfg_key, fc::raw::pack(stretched_seed) );
       std::cerr << "finished saving encrypted seed\n";
  } FC_RETHROW_EXCEPTIONS( warn, "", ("profile_dir",profile_dir)("config",cfg) ) }

  std::wstring profile::get_name()const
  {
    return my->_profile_name;
  }

  void profile::open( const fc::path& profile_dir, const std::string& password )
  { try {
      ilog("opening profile: ${profile_dir}",("profile_dir",profile_dir));
      my->_profile_name = profile_dir.filename().generic_wstring();

      fc::create_directories( profile_dir );
      fc::create_directories( profile_dir / "addressbook" );
      fc::create_directories( profile_dir / "idents" );
      fc::create_directories( profile_dir / "mail" );
      fc::create_directories( profile_dir / "mail" / "inbox" );
      fc::create_directories( profile_dir / "mail" / "draft" );
      fc::create_directories( profile_dir / "mail" / "pending" );
      fc::create_directories( profile_dir / "mail" / "sent" );
      fc::create_directories( profile_dir / "mail" / "smap");
      fc::create_directories( profile_dir / "chat" );
      fc::create_directories( profile_dir / "request" );
      fc::create_directories( profile_dir / "authorization" );

      ilog("loading master key file:" KEYHOTEE_MASTER_KEY_FILE);
      auto profile_cfg_key         = fc::sha512::hash( password.c_str(), password.size() );
      std::vector<char> stretched_seed_data;
      try {
        stretched_seed_data     = fc::aes_load( profile_dir / KEYHOTEE_MASTER_KEY_FILE, profile_cfg_key );
      }
      catch (fc::exception& /* e */)
      { //try to open legacy name for key file
        wlog("Could not open " KEYHOTEE_MASTER_KEY_FILE ", trying to open legacy key file (.strecthed_seed).");
        stretched_seed_data     = fc::aes_load( profile_dir / ".stretched_seed", profile_cfg_key );
      }

      ilog("opening profile databases");
      my->_keychain.set_seed( fc::raw::unpack<fc::sha512>(stretched_seed_data) );
      my->_addressbook->open( profile_dir / "addressbook", profile_cfg_key );
      my->_idents.open( profile_dir / "idents" );
      my->_inbox_db->open( profile_dir / "mail" / "inbox", profile_cfg_key );
      my->_draft_db->open( profile_dir / "mail" / "draft", profile_cfg_key );
      my->_pending_db->open( profile_dir / "mail" / "pending", profile_cfg_key );
      my->_sent_db->open( profile_dir / "mail" / "sent", profile_cfg_key );
      my->_spam_db->open(profile_dir / "mail" / "spam", profile_cfg_key);
      my->_chat_db->open( profile_dir / "chat", profile_cfg_key );
      my->_request_db->open( profile_dir / "request", profile_cfg_key );
      my->_auth_db->open( profile_dir / "authorization", profile_cfg_key );
      my->_last_sync_time.open( profile_dir / "mail" / "last_recv", true );
      if( *my->_last_sync_time == fc::time_point())
      {
          *my->_last_sync_time = fc::time_point::now() - fc::days(5);
          ilog("set last_sync_time to ${t}",("t",*my->_last_sync_time));
      }
      else
          ilog("loaded last_sync_time = ${t}",("t",*my->_last_sync_time));
    ilog("finished opening profile");
  } FC_RETHROW_EXCEPTIONS( warn, "", ("profile_dir",profile_dir) ) }

  std::vector<addressbook::wallet_identity>   profile::identities()const
  { try {
     std::vector<addressbook::wallet_identity> idents;
     for( auto itr = my->_idents.begin(); itr.valid(); ++itr )
     {
       idents.push_back(itr.value());
     }
     return idents;
  } FC_RETHROW_EXCEPTIONS( warn, "" ) }
  
  void    profile::removeIdentity( const std::string& id )
  { try {
      my->_idents.remove( id);
  } FC_RETHROW_EXCEPTIONS( warn, "", ("id",id) ) }

  bool    profile::isIdentityPresent( const std::string& id )
  { try {
      auto itr = my->_idents.find( id);
       return itr.valid();
  } FC_RETHROW_EXCEPTIONS( warn, "", ("id",id) ) }

  void    profile::store_identity( const addressbook::wallet_identity& id )
  { try {
      my->_idents.store( id.dac_id_string, id ); 
  } FC_RETHROW_EXCEPTIONS( warn, "", ("id",id) ) }

  addressbook::wallet_identity    profile::get_identity( const std::string& dac_id_string )const
  { try {
      auto hash_value =  bitname::name_hash(dac_id_string);
      auto idents = profile::identities();
      for ( auto &it : idents ) {
        if (hash_value == bitname::name_hash(it.dac_id_string))
            return it;
      }
      return my->_idents.fetch( dac_id_string ); 
  } FC_RETHROW_EXCEPTIONS( warn, "", ("dac_id",dac_id_string) ) }
  
  /**
   *  Checks the transaction to see if any of the inp
   */
  //void  profile::cache( const bts::blockchain::meta_transaction& mtrx );
  void    profile::cache( const bts::bitchat::decrypted_message& msg    )
  { try {
    //my->_message_db->store( msg );
  } FC_RETHROW_EXCEPTIONS( warn, "", ("msg",msg)) }
  /*
  std::vector<meta_transaction> profile::get_transactions()const
  {
  }
  */

  bitchat::message_db_ptr profile::get_inbox_db() const { return my->_inbox_db; }
  bitchat::message_db_ptr profile::get_draft_db() const { return my->_draft_db; }
  bitchat::message_db_ptr profile::get_pending_db() const { return my->_pending_db; }
  bitchat::message_db_ptr profile::get_sent_db() const { return my->_sent_db; }
  bitchat::message_db_ptr profile::get_spam_db() const { return my->_spam_db; }
  bitchat::message_db_ptr profile::get_chat_db() const { return my->_chat_db; }
  bitchat::message_db_ptr profile::get_request_db() const {return my->_request_db; }
  bitchat::message_db_ptr profile::get_auth_db() const {return my->_auth_db; }

  addressbook::addressbook_ptr profile::get_addressbook() const { return my->_addressbook; }

  keychain profile::get_keychain() const { return my->_keychain; }
  /*
  const std::vector<bitchat::email_draft>&   profile::get_drafts()const
  {
      return my->_drafts;
  }
  void profile::save_draft( const bitchat::email_draft& draft )
  { try {
     
  } FC_RETHROW_EXCEPTIONS( warn, "", ("draft",draft) ) }

  void profile::delete_draft( uint32_t draft_id )
  { try {
     
  } FC_RETHROW_EXCEPTIONS( warn, "", ("draft",draft_id) ) }
*/

} // namespace bts
