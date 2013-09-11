////////////////////////////////////////////////////////////////////////////////
/// @brief database server functionality
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2004-2013 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2013, triagens GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "server.h"

#ifdef _WIN32
#include "BasicsC/win-utils.h"
#endif

#include <regex.h>

#include "BasicsC/conversions.h"
#include "BasicsC/files.h"
#include "BasicsC/hashes.h"
#include "BasicsC/json.h"
#include "BasicsC/locks.h"
#include "BasicsC/logging.h"
#include "BasicsC/memory-map.h"
#include "BasicsC/random.h"
#include "BasicsC/tri-strings.h"
#include "Ahuacatl/ahuacatl-statementlist.h"
#include "VocBase/vocbase.h"

// -----------------------------------------------------------------------------
// --SECTION--                                                   private defines
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief mask value for significant bits of server id
////////////////////////////////////////////////////////////////////////////////

#define TRI_SERVER_ID_MASK 0x0000FFFFFFFFFFFFULL

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief page size
////////////////////////////////////////////////////////////////////////////////

size_t PageSize;

////////////////////////////////////////////////////////////////////////////////
/// @brief random server identifier (16 bit)
////////////////////////////////////////////////////////////////////////////////

static uint16_t ServerIdentifier = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief current tick identifier (48 bit)
////////////////////////////////////////////////////////////////////////////////

static uint64_t CurrentTick = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief tick lock
////////////////////////////////////////////////////////////////////////////////

static TRI_spin_t TickLock;

////////////////////////////////////////////////////////////////////////////////
/// @brief the server's global id
////////////////////////////////////////////////////////////////////////////////

static TRI_server_id_t ServerId;

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                           database hash functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief hashes the database name
////////////////////////////////////////////////////////////////////////////////

static uint64_t HashKeyDatabaseName (TRI_associative_pointer_t* array, 
                                     void const* key) {
  char const* k = (char const*) key;

  return TRI_FnvHashString(k);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief hashes the database name
////////////////////////////////////////////////////////////////////////////////

static uint64_t HashElementDatabaseName (TRI_associative_pointer_t* array, 
                                         void const* element) {
  TRI_vocbase_t const* e = element;
  char const* name = e->_name;

  return TRI_FnvHashString(name);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief compares a database name and a database
////////////////////////////////////////////////////////////////////////////////

static bool EqualKeyDatabaseName (TRI_associative_pointer_t* array, 
                                  void const* key, 
                                  void const* element) {
  char const* k = (char const*) key;
  TRI_vocbase_t const* e = element;

  return TRI_EqualString(k, e->_name);
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                    tick functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the current tick value, without using a lock
////////////////////////////////////////////////////////////////////////////////

static inline TRI_voc_tick_t GetTick (void) {
  return (ServerIdentifier | (CurrentTick << 16));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief updates the tick counter, without using a lock
////////////////////////////////////////////////////////////////////////////////

static inline void UpdateTick (TRI_voc_tick_t tick) {
  TRI_voc_tick_t s = tick >> 16;

  if (CurrentTick < s) {
    CurrentTick = s;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief extract the numeric part from a filename
////////////////////////////////////////////////////////////////////////////////

static uint64_t GetNumericFilenamePart (const char* filename) {
  char* pos;

  pos = strrchr(filename, '-');

  if (pos == NULL) {
    return 0;
  }

  return TRI_UInt64String(pos + 1);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief compare two filenames, based on the numeric part contained in
/// the filename. this is used to sort database filenames on startup 
////////////////////////////////////////////////////////////////////////////////

static int NameComparator (const void* lhs, const void* rhs) {
  const char* l = *((char**) lhs);
  const char* r = *((char**) rhs);

  const uint64_t numLeft  = GetNumericFilenamePart(l);
  const uint64_t numRight = GetNumericFilenamePart(r);

  if (numLeft != numRight) {
    return numLeft < numRight ? -1 : 1;
  }

  return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief generates a new server id
///
/// TODO: generate a real UUID instead of the 2 random values
////////////////////////////////////////////////////////////////////////////////

static int GenerateServerId (void) {
  uint64_t randomValue = 0ULL; // init for our friend Valgrind
  uint32_t value1, value2;

  // save two uint32_t values
  value1 = TRI_UInt32Random();
  value2 = TRI_UInt32Random();

  // use the lower 6 bytes only
  randomValue = (((uint64_t) value1) << 32) | ((uint64_t) value2);
  randomValue &= TRI_SERVER_ID_MASK;

  ServerId = (TRI_server_id_t) randomValue;

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief reads server id from file
////////////////////////////////////////////////////////////////////////////////

static int ReadServerId (char const* filename) {
  TRI_json_t* json;
  TRI_json_t* idString;
  TRI_server_id_t foundId;

  assert(filename != NULL);

  if (! TRI_ExistsFile(filename)) {
    return TRI_ERROR_FILE_NOT_FOUND;
  }

  json = TRI_JsonFile(TRI_UNKNOWN_MEM_ZONE, filename, NULL);

  if (json == NULL) {
    return TRI_ERROR_INTERNAL;
  }

  idString = TRI_LookupArrayJson(json, "serverId");

  if (! TRI_IsStringJson(idString)) {
    TRI_FreeJson(TRI_UNKNOWN_MEM_ZONE, json);
    return TRI_ERROR_INTERNAL;
  }

  foundId = TRI_UInt64String2(idString->_value._string.data,
                              idString->_value._string.length - 1);

  LOG_TRACE("using existing server id: %llu", (unsigned long long) foundId);
  TRI_FreeJson(TRI_UNKNOWN_MEM_ZONE, json);

  if (foundId == 0) {
    return TRI_ERROR_INTERNAL;
  }

  ServerId = foundId;

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief writes server id to file
////////////////////////////////////////////////////////////////////////////////

static int WriteServerId (char const* filename) {
  TRI_json_t* json;
  char* idString;
  char buffer[32];
  size_t len;
  time_t tt;
  struct tm tb;
  bool ok;

  assert(filename != NULL);

  // create a json object
  json = TRI_CreateArrayJson(TRI_CORE_MEM_ZONE);

  if (json == NULL) {
    // out of memory
    LOG_ERROR("cannot save server id in file '%s': out of memory", filename);
    return TRI_ERROR_OUT_OF_MEMORY;
  }

  assert(ServerId != 0);

  idString = TRI_StringUInt64((uint64_t) ServerId);
  TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, json, "serverId", TRI_CreateStringCopyJson(TRI_CORE_MEM_ZONE, idString));
  TRI_FreeString(TRI_CORE_MEM_ZONE, idString);

  tt = time(0);
  TRI_gmtime(tt, &tb);
  len = strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tb);
  TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, json, "createdTime", TRI_CreateString2CopyJson(TRI_CORE_MEM_ZONE, buffer, len));

  // save json info to file
  LOG_DEBUG("Writing server id to file '%s'", filename);
  ok = TRI_SaveJson(filename, json, true);
  TRI_FreeJson(TRI_CORE_MEM_ZONE, json);

  if (! ok) {
    LOG_ERROR("could not save server id in file '%s': %s", filename, TRI_last_error());

    return TRI_ERROR_INTERNAL;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief read / create the server id on startup
////////////////////////////////////////////////////////////////////////////////

static int DetermineServerId (TRI_server_t* server) {
  int res;

  res = ReadServerId(server->_serverIdFilename);

  if (res == TRI_ERROR_FILE_NOT_FOUND) {
    // id file does not yet exist. now create it
    res = GenerateServerId();

    if (res == TRI_ERROR_NO_ERROR) {
      // id was generated. now save it
      res = WriteServerId(server->_serverIdFilename);
    }
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief reads shutdown information file
/// this is called at server startup. if the file is present, the last tick
/// value used by the server will be read from the file.
////////////////////////////////////////////////////////////////////////////////

static int ReadShutdownInfo (char const* filename) {
  TRI_json_t* json;
  TRI_json_t* shutdownTime;
  TRI_json_t* tickString;
  uint64_t foundTick;

  assert(filename != NULL);

  if (! TRI_ExistsFile(filename)) {
    return TRI_ERROR_FILE_NOT_FOUND;
  }

  json = TRI_JsonFile(TRI_CORE_MEM_ZONE, filename, NULL);

  if (json == NULL) {
    return TRI_ERROR_INTERNAL;
  }
  
  shutdownTime = TRI_LookupArrayJson(json, "shutdownTime");

  if (TRI_IsStringJson(shutdownTime)) {
    LOG_DEBUG("server was shut down cleanly last time at '%s'", shutdownTime->_value._string.data);
  }

  tickString = TRI_LookupArrayJson(json, "tick");

  if (! TRI_IsStringJson(tickString)) {
    TRI_FreeJson(TRI_CORE_MEM_ZONE, json);

    return TRI_ERROR_INTERNAL;
  }

  foundTick = TRI_UInt64String2(tickString->_value._string.data,
                                tickString->_value._string.length - 1);
  TRI_FreeJson(TRI_CORE_MEM_ZONE, json);

  LOG_TRACE("using existing tick from shutdown info file: %llu", 
            (unsigned long long) foundTick);

  if (foundTick == 0) {
    return TRI_ERROR_INTERNAL;
  }

  UpdateTick((TRI_voc_tick_t) foundTick);

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief removes the shutdown information file
/// this is called after the shutdown info file is read at restart. we need
/// to remove the file because if we don't and the server crashes, we would
/// leave some stale data around, leading to potential inconsistencies later.
////////////////////////////////////////////////////////////////////////////////

static int RemoveShutdownInfo (TRI_server_t* server) {
  int res = TRI_UnlinkFile(server->_shutdownFilename);

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief writes shutdown information file
/// the file will contain the timestamp of the shutdown time plus the last
/// tick value the server used. it will be read on restart of the server.
/// if the server can find the file on restart, it can avoid scanning 
/// collections.
////////////////////////////////////////////////////////////////////////////////

static int WriteShutdownInfo (TRI_server_t* server) {
  TRI_json_t* json;
  char* tickString;
  char buffer[32];
  size_t len;
  time_t tt;
  struct tm tb;
  bool ok;

  // create a json object
  json = TRI_CreateArrayJson(TRI_CORE_MEM_ZONE);

  if (json == NULL) {
    // out of memory
    LOG_ERROR("cannot save shutdown info in file '%s': out of memory", 
              server->_shutdownFilename);

    return TRI_ERROR_OUT_OF_MEMORY;
  }

  tickString = TRI_StringUInt64((uint64_t) GetTick());
  TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, json, "tick", TRI_CreateStringCopyJson(TRI_CORE_MEM_ZONE, tickString));
  TRI_FreeString(TRI_CORE_MEM_ZONE, tickString);

  tt = time(0);
  TRI_gmtime(tt, &tb);
  len = strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tb);
  TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, json, "shutdownTime", TRI_CreateString2CopyJson(TRI_CORE_MEM_ZONE, buffer, len));

  // save json info to file
  LOG_DEBUG("writing shutdown info to file '%s'", server->_shutdownFilename);
  ok = TRI_SaveJson(server->_shutdownFilename, json, true);
  TRI_FreeJson(TRI_CORE_MEM_ZONE, json);

  if (! ok) {
    LOG_ERROR("could not save shutdown info in file '%s': %s", 
              server->_shutdownFilename, 
              TRI_last_error());

    return TRI_ERROR_INTERNAL;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief iterate over all databases in the databases directory and open them
////////////////////////////////////////////////////////////////////////////////

static int OpenDatabases (TRI_server_t* server) {
  TRI_vector_string_t files;
  size_t i, n;
  int res;

  res = TRI_ERROR_NO_ERROR;
  files = TRI_FilesDirectory(server->_databasePath);
  n = files._length;

  for (i = 0;  i < n;  ++i) {
    TRI_vocbase_t* vocbase;
    TRI_json_t* json;
    TRI_json_t const* deletedJson;
    TRI_json_t const* nameJson;
    TRI_vocbase_defaults_t defaults;
    char const* name;
    char* databaseDirectory;
    char* parametersFile;
    char* error;
    char* databaseName;
    void const* found;

    name = files._buffer[i];
    assert(name != NULL);
  
    // .............................................................................
    // construct and validate path
    // .............................................................................

    databaseDirectory = TRI_Concatenate2File(server->_databasePath, name);

    if (databaseDirectory == NULL) {
      res = TRI_ERROR_OUT_OF_MEMORY;
      break;
    }

    if (! TRI_IsDirectory(databaseDirectory)) {
      TRI_FreeString(TRI_CORE_MEM_ZONE, databaseDirectory);
      continue;
    }

    // we have a directory...
    
    if (! TRI_IsWritable(databaseDirectory)) {
      // the database directory we found is not writable for the current user
      // this can cause serious trouble so we will abort the server start if we
      // encounter this situation
      LOG_ERROR("database directory '%s' is not writable for current user", 
                databaseDirectory);

      TRI_FreeString(TRI_CORE_MEM_ZONE, databaseDirectory);
      res = TRI_ERROR_ARANGO_DATADIR_NOT_WRITABLE;
      break;
    }

    // we have a writable directory...
    
    // .............................................................................
    // read parameter.json file
    // .............................................................................

    // now read data from parameter.json file
    parametersFile = TRI_Concatenate2File(databaseDirectory, TRI_VOC_PARAMETER_FILE);

    if (parametersFile == NULL) {
      TRI_FreeString(TRI_CORE_MEM_ZONE, databaseDirectory);
      res = TRI_ERROR_OUT_OF_MEMORY;
      break;
    }

    if (! TRI_ExistsFile(parametersFile)) {
      // no parameter.json file
      LOG_ERROR("database directory '%s' does not contain parameters file",
                databaseDirectory);

      TRI_FreeString(TRI_CORE_MEM_ZONE, parametersFile);
      TRI_FreeString(TRI_CORE_MEM_ZONE, databaseDirectory);
      // skip this database
      continue;
    }

    LOG_DEBUG("reading database parameters from file '%s'",
              parametersFile);

    error = NULL;
    json = TRI_JsonFile(TRI_CORE_MEM_ZONE, parametersFile, &error);

    if (json == NULL) {
      if (error != NULL) {
        TRI_FreeString(TRI_CORE_MEM_ZONE, error);
      }

      LOG_ERROR("database directory '%s' does not contain a valid parameters file", 
                databaseDirectory);
      
      TRI_FreeString(TRI_CORE_MEM_ZONE, parametersFile);
      TRI_FreeString(TRI_CORE_MEM_ZONE, databaseDirectory);
      // skip this database
      continue;
    }

    TRI_FreeString(TRI_CORE_MEM_ZONE, parametersFile);

    deletedJson = TRI_LookupArrayJson(json, "deleted");

    if (TRI_IsBooleanJson(deletedJson)) {
      if (deletedJson->_value._boolean) {
        // database is deleted, skip it!
        TRI_FreeString(TRI_CORE_MEM_ZONE, databaseDirectory);
        TRI_FreeJson(TRI_CORE_MEM_ZONE, json);
        continue;
      }
    }

    nameJson = TRI_LookupArrayJson(json, "name");

    if (! TRI_IsStringJson(nameJson)) {
      LOG_ERROR("database directory '%s' does not contain a valid parameters file", 
                databaseDirectory);

      TRI_FreeString(TRI_CORE_MEM_ZONE, databaseDirectory);
      TRI_FreeJson(TRI_CORE_MEM_ZONE, json);
      // skip this database
      continue;
    }
    
    databaseName = TRI_DuplicateString2Z(TRI_CORE_MEM_ZONE, nameJson->_value._string.data, nameJson->_value._string.length);
    
    // .............................................................................
    // setup defaults
    // .............................................................................

    // use defaults and blend them with parameters found in file
    TRI_GetDatabaseDefaultsServer(server, &defaults);
    TRI_FromJsonVocBaseDefaults(&defaults, TRI_LookupArrayJson(json, "properties"));
    

    TRI_FreeJson(TRI_CORE_MEM_ZONE, json);

    if (databaseName == NULL) {
      TRI_FreeString(TRI_CORE_MEM_ZONE, databaseDirectory);
      res = TRI_ERROR_OUT_OF_MEMORY;
      break;
    }

    // .............................................................................
    // open the database and scan collections in it
    // .............................................................................

    // try to open this database
    vocbase = TRI_OpenVocBase(databaseDirectory, databaseName, &defaults, server->_wasShutdownCleanly);

    TRI_FreeString(TRI_CORE_MEM_ZONE, databaseName);

    if (vocbase == NULL) {
      // grab last error
      res = TRI_errno();

      if (res != TRI_ERROR_NO_ERROR) {
        // but we must have an error...
        res = TRI_ERROR_INTERNAL;
      }

      LOG_ERROR("could not process database directory '%s' for database '%s': %s", 
                databaseDirectory,
                name, 
                TRI_errno_string(res));

      TRI_FreeString(TRI_CORE_MEM_ZONE, databaseDirectory);
      break;
    }
    
    // we found a valid database  
    TRI_FreeString(TRI_CORE_MEM_ZONE, databaseDirectory);

    res = TRI_InsertKeyAssociativePointer2(&server->_databases, vocbase->_name, vocbase, &found);

    if (res != TRI_ERROR_NO_ERROR) {
      LOG_ERROR("could not add database '%s': out of memory", 
                name);

      break;
    }

    // should never have a duplicate database name
    assert(found == NULL);
  
    LOG_INFO("loaded database '%s' from '%s'",
             vocbase->_name,
             vocbase->_path);
  }
  
  TRI_DestroyVectorString(&files);

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief close all opened databases
////////////////////////////////////////////////////////////////////////////////

static int CloseDatabases (TRI_server_t* server) {
  size_t i, n;

  n = server->_databases._nrAlloc;

  for (i = 0; i < n; ++i) {
    TRI_vocbase_t* vocbase = server->_databases._table[i];

    if (vocbase != NULL) {
      TRI_DestroyVocBase(vocbase, NULL);
      TRI_Free(TRI_UNKNOWN_MEM_ZONE, vocbase);

      // clear to avoid potential double freeing
      server->_databases._table[i] = NULL;
    }
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get the names of all databases in the ArangoDB 1.4 layout
////////////////////////////////////////////////////////////////////////////////

static int GetDatabases (TRI_server_t* server, 
                         TRI_vector_string_t* databases) {
  regex_t re;
  regmatch_t matches[2];
  TRI_vector_string_t files;
  int res;
  size_t i, n;
  
  assert(server != NULL);

  res = regcomp(&re, "^database-([0-9][0-9]*)$", REG_EXTENDED);

  if (res != 0) {
    LOG_ERROR("unable to compile regular expression");

    return TRI_ERROR_INTERNAL;
  }

  res = TRI_ERROR_NO_ERROR;
  files = TRI_FilesDirectory(server->_databasePath);
  n = files._length;

  for (i = 0;  i < n;  ++i) {
    char const* name;
    char* dname;

    name = files._buffer[i];
    assert(name != NULL);

    if (regexec(&re, name, sizeof(matches) / sizeof(matches[0]), matches, 0) != 0) {
      // found some other file 
      continue;
    }

    // found a database name
    
    dname = TRI_Concatenate2File(server->_databasePath, name);

    if (dname == NULL) {
      res = TRI_ERROR_OUT_OF_MEMORY;
      break;
    }

    if (TRI_IsDirectory(dname)) {
      TRI_PushBackVectorString(databases, TRI_DuplicateStringZ(TRI_CORE_MEM_ZONE, name));
    }

    TRI_FreeString(TRI_CORE_MEM_ZONE, dname);
  }

  TRI_DestroyVectorString(&files);
  regfree(&re);
 
  // sort by id 
  qsort(databases->_buffer, databases->_length, sizeof(char*), &NameComparator);

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief move collections from the main data directory into the system 
/// database subdirectory
////////////////////////////////////////////////////////////////////////////////

static int MoveOldCollections (TRI_server_t* server,
                               char const* systemName) {
  regex_t re;
  regmatch_t matches[2];
  TRI_vector_string_t files;
  int res;
  size_t i, n;

  assert(server != NULL);
  assert(systemName != NULL);

  res = regcomp(&re, "^collection-([0-9][0-9]*)$", REG_EXTENDED);

  if (res != 0) {
    LOG_ERROR("unable to compile regular expression");

    return TRI_ERROR_INTERNAL;
  }

  res = TRI_ERROR_NO_ERROR;
  files = TRI_FilesDirectory(server->_basePath);
  n = files._length;

  for (i = 0;  i < n;  ++i) {
    char const* name;
    char* oldName;
    char* targetName;

    name = files._buffer[i];
    assert(name != NULL);

    if (regexec(&re, name, sizeof(matches) / sizeof(matches[0]), matches, 0) != 0) {
      // found something else than "collection-xxxx". we can ignore these files/directories
      continue;
    }

    oldName = TRI_Concatenate2File(server->_basePath, name);

    if (oldName == NULL) {
      res = TRI_ERROR_OUT_OF_MEMORY;
      break;
    }

    if (! TRI_IsDirectory(oldName)) {
      // not a directory
      TRI_FreeString(TRI_CORE_MEM_ZONE, oldName);
      continue;
    }

    // move into system database directory

    targetName = TRI_Concatenate3File(server->_databasePath, systemName, name);
    
    if (targetName == NULL) {
      TRI_FreeString(TRI_CORE_MEM_ZONE, oldName);
      res = TRI_ERROR_OUT_OF_MEMORY;
      break;
    }

    LOG_INFO("moving standalone collection directory from '%s' to system database directory '%s'",
             oldName,
             targetName);

    // rename directory
    res = TRI_RenameFile(oldName, targetName);

    TRI_FreeString(TRI_CORE_MEM_ZONE, oldName);
    TRI_FreeString(TRI_CORE_MEM_ZONE, targetName);

    if (res != TRI_ERROR_NO_ERROR) {
      LOG_ERROR("moving collection directory failed: %s",
                TRI_errno_string(res));
      break;
    }
  }

  TRI_DestroyVectorString(&files);
  regfree(&re);

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief save a parameter.json file for a database
////////////////////////////////////////////////////////////////////////////////

static int SaveDatabaseParameters (TRI_voc_tick_t id,
                                   char const* name,
                                   bool deleted,
                                   TRI_vocbase_defaults_t const* defaults,
                                   char const* directory) {
  char* file;
  char* tickString;
  TRI_json_t* json;
  TRI_json_t* properties;
  
  assert(id > 0);
  assert(name != NULL);
  assert(directory != NULL);
    
  file = TRI_Concatenate2File(directory, TRI_VOC_PARAMETER_FILE);

  if (file == NULL) {
    return TRI_ERROR_OUT_OF_MEMORY;
  }
    
  tickString = TRI_StringUInt64((uint64_t) id);

  if (tickString == NULL) {
    TRI_FreeString(TRI_CORE_MEM_ZONE, file);

    return TRI_ERROR_OUT_OF_MEMORY;
  }
    
  json = TRI_CreateArrayJson(TRI_CORE_MEM_ZONE);

  if (json == NULL) {
    TRI_FreeString(TRI_CORE_MEM_ZONE, tickString);
    TRI_FreeString(TRI_CORE_MEM_ZONE, file);

    return TRI_ERROR_OUT_OF_MEMORY;
  }
  
  properties = TRI_JsonVocBaseDefaults(TRI_CORE_MEM_ZONE, defaults);

  if (properties == NULL) {
    TRI_FreeJson(TRI_CORE_MEM_ZONE, json);
    TRI_FreeString(TRI_CORE_MEM_ZONE, tickString);
    TRI_FreeString(TRI_CORE_MEM_ZONE, file);

    return TRI_ERROR_OUT_OF_MEMORY;
  }

  TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, json, "id", TRI_CreateStringCopyJson(TRI_CORE_MEM_ZONE, tickString));
  TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, json, "name", TRI_CreateStringCopyJson(TRI_CORE_MEM_ZONE, name));
  TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, json, "deleted", TRI_CreateBooleanJson(TRI_CORE_MEM_ZONE, deleted));
  TRI_Insert3ArrayJson(TRI_CORE_MEM_ZONE, json, "properties", properties);
    
  TRI_FreeString(TRI_CORE_MEM_ZONE, tickString);
    
  if (! TRI_SaveJson(file, json, true)) {
    LOG_ERROR("cannot save database information in file '%s'",
              file);

    TRI_FreeJson(TRI_CORE_MEM_ZONE, json);
    TRI_FreeString(TRI_CORE_MEM_ZONE, file);

    return TRI_ERROR_INTERNAL;
  }
    
  TRI_FreeJson(TRI_CORE_MEM_ZONE, json);
  TRI_FreeString(TRI_CORE_MEM_ZONE, file);

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create a new database directory and return its name
////////////////////////////////////////////////////////////////////////////////

static int CreateDatabaseDirectory (TRI_server_t* server, 
                                    char const* databaseName,
                                    TRI_vocbase_defaults_t const* defaults,
                                    char** name) {
  char* tickString;
  char* dname;
  char* file;
  TRI_voc_tick_t tick;
  int res;
  
  assert(server != NULL);
  assert(databaseName != NULL);

  tick = TRI_NewTickServer();
  tickString = TRI_StringUInt64(tick);

  if (tickString == NULL) {
    return TRI_ERROR_OUT_OF_MEMORY;
  }

  dname = TRI_Concatenate2String("database-", tickString);
  TRI_FreeString(TRI_CORE_MEM_ZONE, tickString);

  if (dname == NULL) {
    return TRI_ERROR_OUT_OF_MEMORY;
  }

  file = TRI_Concatenate2File(server->_databasePath, dname);

  if (file == NULL) {
    TRI_FreeString(TRI_CORE_MEM_ZONE, dname);

    return TRI_ERROR_OUT_OF_MEMORY;
  }

  res = TRI_CreateDirectory(file);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_FreeString(TRI_CORE_MEM_ZONE, file);
    TRI_FreeString(TRI_CORE_MEM_ZONE, dname);

    return res;
  }

  res = SaveDatabaseParameters(tick, databaseName, false, defaults, file);
  TRI_FreeString(TRI_CORE_MEM_ZONE, file);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  // takes ownership of the string
  *name = dname;

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief move 1.4-alpha database directories around until they are matching 
/// the final ArangoDB 1.4 filename layout
////////////////////////////////////////////////////////////////////////////////

static int Move14AlphaDatabases (TRI_server_t* server) { 
  regex_t re;
  regmatch_t matches[2];
  TRI_vector_string_t files;
  int res;
  size_t i, n;
  
  assert(server != NULL);

  res = regcomp(&re, "^database-([0-9][0-9]*)$", REG_EXTENDED);

  if (res != 0) {
    LOG_ERROR("unable to compile regular expression");

    return TRI_ERROR_INTERNAL;
  }

  res = TRI_ERROR_NO_ERROR;
  files = TRI_FilesDirectory(server->_databasePath);
  n = files._length;

  for (i = 0;  i < n;  ++i) {
    char const* name;
    char* tickString;
    char* dname;
    char* targetName;
    char* oldName;
    TRI_voc_tick_t tick;

    name = files._buffer[i];
    assert(name != NULL);

    if (regexec(&re, name, sizeof(matches) / sizeof(matches[0]), matches, 0) == 0) {
      // found "database-xxxx". this is the desired format already
      continue;
    }

    // found some other format. we need to adjust the name
    
    oldName = TRI_Concatenate2File(server->_databasePath, name);

    if (oldName == NULL) {
      res = TRI_ERROR_OUT_OF_MEMORY;
      break;
    }

    if (! TRI_IsDirectory(oldName)) {
      // found a non-directory
      TRI_FreeString(TRI_CORE_MEM_ZONE, oldName);
      continue;
    }

    tick = TRI_NewTickServer();
    tickString = TRI_StringUInt64(tick);

    if (tickString == NULL) {
      return TRI_ERROR_OUT_OF_MEMORY;
    }

    dname = TRI_Concatenate2String("database-", tickString);
    TRI_FreeString(TRI_CORE_MEM_ZONE, tickString);

    if (dname == NULL) {
      TRI_FreeString(TRI_CORE_MEM_ZONE, oldName);
      res = TRI_ERROR_OUT_OF_MEMORY;
    }

    targetName = TRI_Concatenate2File(server->_databasePath, dname);
    TRI_FreeString(TRI_CORE_MEM_ZONE, dname);

    if (targetName == NULL) {
      TRI_FreeString(TRI_CORE_MEM_ZONE, oldName);
      res = TRI_ERROR_OUT_OF_MEMORY;
      break;
    }
    
    res = SaveDatabaseParameters(tick, name, false, &server->_defaults, oldName);

    if (res != TRI_ERROR_NO_ERROR) {
      TRI_FreeString(TRI_CORE_MEM_ZONE, targetName);
      TRI_FreeString(TRI_CORE_MEM_ZONE, oldName);
      break;
    }

    LOG_INFO("renaming database directory from '%s' to '%s'",
             oldName,
             targetName);

    res = TRI_RenameFile(oldName, targetName);

    TRI_FreeString(TRI_CORE_MEM_ZONE, oldName);
    TRI_FreeString(TRI_CORE_MEM_ZONE, targetName);

    if (res != TRI_ERROR_NO_ERROR) {
      LOG_ERROR("renaming database failed: %s",
                TRI_errno_string(res));
      break;
    }
  }

  TRI_DestroyVectorString(&files);
  regfree(&re);

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief initialise the list of databases
////////////////////////////////////////////////////////////////////////////////

static int InitDatabases (TRI_server_t* server) {
  TRI_vector_string_t names;
  int res;
  
  assert(server != NULL);
  
  TRI_InitVectorString(&names, TRI_CORE_MEM_ZONE);

  res = GetDatabases(server, &names);

  if (res == TRI_ERROR_NO_ERROR) {
    if (names._length == 0) {
      char* name;

      // no databases found, i.e. there is no system database!
      // create a database for the system database
      res = CreateDatabaseDirectory(server, TRI_VOC_SYSTEM_DATABASE, &server->_defaults, &name);

      if (res == TRI_ERROR_NO_ERROR) {
        if (TRI_PushBackVectorString(&names, name) != TRI_ERROR_NO_ERROR) {
          TRI_FreeString(TRI_CORE_MEM_ZONE, name);
          res = TRI_ERROR_OUT_OF_MEMORY;
        }
      }
    }

    if (res == TRI_ERROR_NO_ERROR) {
      char const* systemName;

      assert(names._length > 0);

      systemName = names._buffer[0];

      // this performs a migration of the collections of the "only" pre-1.4
      // database into the system database and its own directory
      res = MoveOldCollections(server, systemName);

      if (res == TRI_ERROR_NO_ERROR) {
        // this renames database directories created with 1.4-alpha from the
        // database name to "database-xxx"
        res = Move14AlphaDatabases(server);
      }
    }
  }

  TRI_DestroyVectorString(&names);

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief create a server instance
////////////////////////////////////////////////////////////////////////////////

TRI_server_t* TRI_CreateServer () {
  TRI_server_t* server;

  server = TRI_Allocate(TRI_CORE_MEM_ZONE, sizeof(TRI_server_t), false);

  return server;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief initialise a server instance with configuration
////////////////////////////////////////////////////////////////////////////////

int TRI_InitServer (TRI_server_t* server,
                    char const* basePath,
                    TRI_vocbase_defaults_t const* defaults) {

  assert(server != NULL);

  server->_basePath = TRI_DuplicateStringZ(TRI_CORE_MEM_ZONE, basePath);

  if (server->_basePath == NULL) {
    return TRI_ERROR_OUT_OF_MEMORY;
  }
  
  server->_databasePath = TRI_Concatenate2File(server->_basePath, "databases");

  if (server->_databasePath == NULL) {
    TRI_Free(TRI_CORE_MEM_ZONE, server->_basePath);

    return TRI_ERROR_OUT_OF_MEMORY;
  }
  
  server->_lockFilename = TRI_Concatenate2File(server->_basePath, "LOCK");

  if (server->_lockFilename == NULL) {
    TRI_Free(TRI_CORE_MEM_ZONE, server->_databasePath);
    TRI_Free(TRI_CORE_MEM_ZONE, server->_basePath);

    return TRI_ERROR_OUT_OF_MEMORY;
  }
  
  server->_shutdownFilename = TRI_Concatenate2File(server->_basePath, "SHUTDOWN");

  if (server->_shutdownFilename == NULL) {
    TRI_Free(TRI_CORE_MEM_ZONE, server->_lockFilename);
    TRI_Free(TRI_CORE_MEM_ZONE, server->_databasePath);
    TRI_Free(TRI_CORE_MEM_ZONE, server->_basePath);

    return TRI_ERROR_OUT_OF_MEMORY;
  }
  
  server->_serverIdFilename = TRI_Concatenate2File(server->_basePath, "SERVER");

  if (server->_serverIdFilename == NULL) {
    TRI_Free(TRI_CORE_MEM_ZONE, server->_shutdownFilename);
    TRI_Free(TRI_CORE_MEM_ZONE, server->_lockFilename);
    TRI_Free(TRI_CORE_MEM_ZONE, server->_databasePath);
    TRI_Free(TRI_CORE_MEM_ZONE, server->_basePath);

    return TRI_ERROR_OUT_OF_MEMORY;
  }

  // copy defaults
  memcpy(&server->_defaults, defaults, sizeof(TRI_vocbase_defaults_t));
  
  TRI_InitAssociativePointer(&server->_databases,
                             TRI_UNKNOWN_MEM_ZONE,
                             HashKeyDatabaseName,
                             HashElementDatabaseName,
                             EqualKeyDatabaseName,
                             NULL);
  
  TRI_InitReadWriteLock(&server->_lock);

  TRI_InitMutex(&server->_createLock);

  server->_wasShutdownCleanly = false;

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy a server instance
////////////////////////////////////////////////////////////////////////////////

void TRI_DestroyServer (TRI_server_t* server) {
  CloseDatabases(server);

  TRI_DestroyMutex(&server->_createLock);
  TRI_DestroyReadWriteLock(&server->_lock);
  TRI_DestroyAssociativePointer(&server->_databases);

  TRI_Free(TRI_CORE_MEM_ZONE, server->_serverIdFilename);
  TRI_Free(TRI_CORE_MEM_ZONE, server->_shutdownFilename);
  TRI_Free(TRI_CORE_MEM_ZONE, server->_lockFilename);
  TRI_Free(TRI_CORE_MEM_ZONE, server->_databasePath);
  TRI_Free(TRI_CORE_MEM_ZONE, server->_basePath);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief free a server instance
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeServer (TRI_server_t* server) {
  TRI_DestroyServer(server);
  TRI_Free(TRI_CORE_MEM_ZONE, server);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief initialise globals
////////////////////////////////////////////////////////////////////////////////

void TRI_InitialiseServer () {
  ServerIdentifier = TRI_UInt16Random();
  PageSize         = (size_t) getpagesize();
  
  memset(&ServerId, 0, sizeof(TRI_server_id_t));

  TRI_InitSpin(&TickLock);
  
  TRI_GlobalInitStatementListAql();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief de-initialise globals
////////////////////////////////////////////////////////////////////////////////

void TRI_ShutdownServer () {
  TRI_GlobalFreeStatementListAql();
  TRI_DestroySpin(&TickLock);
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief get the global server id
////////////////////////////////////////////////////////////////////////////////

TRI_server_id_t TRI_GetIdServer () {
  return ServerId;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief start the server
////////////////////////////////////////////////////////////////////////////////

int TRI_StartServer (TRI_server_t* server) {
  int res;

  if (! TRI_IsDirectory(server->_basePath)) {
    LOG_ERROR("database path '%s' is not a directory", 
              server->_basePath);

    return TRI_ERROR_ARANGO_DATADIR_INVALID;
  }
  
  if (! TRI_IsWritable(server->_basePath)) {
    // database directory is not writable for the current user... bad luck
    LOG_ERROR("database directory '%s' is not writable for current user", 
              server->_basePath);

    return TRI_ERROR_ARANGO_DATADIR_NOT_WRITABLE;
  }
  
  // .............................................................................
  // check that the database is not locked and lock it
  // .............................................................................

  res = TRI_VerifyLockFile(server->_lockFilename);

  if (res == TRI_ERROR_NO_ERROR) {
    LOG_ERROR("database is locked, please check the lock file '%s'", 
              server->_lockFilename);

    return TRI_ERROR_ARANGO_DATADIR_LOCKED;
  }

  if (TRI_ExistsFile(server->_lockFilename)) {
    TRI_UnlinkFile(server->_lockFilename);
  }

  res = TRI_CreateLockFile(server->_lockFilename);

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_ERROR("cannot lock the database directory, please check the lock file '%s': %s",
              server->_lockFilename, 
              TRI_errno_string(res));

    return TRI_ERROR_ARANGO_DATADIR_UNLOCKABLE;
  }


  // .............................................................................
  // read the server id
  // .............................................................................

  res = DetermineServerId(server);

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_ERROR("reading/creating server file failed: %s",
              TRI_errno_string(res));

    return res;
  }
  
  
  // .............................................................................
  // read information from last shutdown
  // .............................................................................

  // check if we can find a SHUTDOWN file
  // this file will contain the last tick value issued by the server
  // if we find the file, we can avoid scanning datafiles for the last used tick value

  res = ReadShutdownInfo(server->_shutdownFilename);

  if (res == TRI_ERROR_INTERNAL) {
    LOG_ERROR("cannot read shutdown information from file '%s'", 
              server->_shutdownFilename);

    return TRI_ERROR_INTERNAL;
  }

  server->_wasShutdownCleanly = (res == TRI_ERROR_NO_ERROR);
 
  
  // .............................................................................
  // verify existence of "databases" subdirectory
  // .............................................................................

  if (! TRI_IsDirectory(server->_databasePath)) {
    res = TRI_CreateDirectory(server->_databasePath);

    if (res != TRI_ERROR_NO_ERROR) {
      LOG_ERROR("unable to create database directory '%s': %s", 
                server->_databasePath,
                TRI_errno_string(res));

      return TRI_ERROR_ARANGO_DATADIR_NOT_WRITABLE;
    }
  }

  if (! TRI_IsWritable(server->_databasePath)) {
    LOG_ERROR("database drectory '%s' is not writable", 
              server->_databasePath);

    return TRI_ERROR_ARANGO_DATADIR_NOT_WRITABLE;
  }

  
  // .............................................................................
  // perform an eventual migration of the databases. 
  // .............................................................................

  res = InitDatabases(server);

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_ERROR("unable to initialise databases: %s", 
              TRI_errno_string(res));
    return res;
  }


  // .............................................................................
  // open and scan all databases
  // .............................................................................
  
  // scan all databases
  res = OpenDatabases(server);

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_ERROR("could not iterate over all databases: %s",
              TRI_errno_string(res));

    return res;
  }

  LOG_TRACE("last tick value found: %llu", (unsigned long long) GetTick());  

  // now remove SHUTDOWN file if it was present
  if (server->_wasShutdownCleanly) {
    res = RemoveShutdownInfo(server);

    if (res != TRI_ERROR_NO_ERROR) {
      LOG_ERROR("unable to remove shutdown information file '%s': %s", 
                server->_shutdownFilename,
                TRI_errno_string(res));

      return res;
    }
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief stop the server
////////////////////////////////////////////////////////////////////////////////

int TRI_StopServer (TRI_server_t* server) {
  CloseDatabases(server);

  // we are just before terminating the server. we can now write out a file with the
  // shutdown timestamp and the last tick value the server used.
  // if writing the file fails, it is not a problem as in this case we'll scan the
  // collections for the tick value on startup
  WriteShutdownInfo(server);
  TRI_DestroyLockFile(server->_lockFilename);

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create a new database 
////////////////////////////////////////////////////////////////////////////////

int TRI_CreateDatabaseServer (TRI_server_t* server,
                              char const* name,
                              TRI_vocbase_defaults_t const* defaults,
                              TRI_vocbase_t** database) {
  TRI_vocbase_t* vocbase;
  char* file;
  char* path;
  int res;
  size_t i, n;
  
  if (! TRI_IsAllowedDatabaseName(false, name)) {
    return TRI_ERROR_ARANGO_DATABASE_NAME_INVALID;
  }

  TRI_LockMutex(&server->_createLock);

  TRI_ReadLockReadWriteLock(&server->_lock);

  n = server->_databases._nrAlloc;
  for (i = 0; i < n; ++i) {
    vocbase = server->_databases._table[i];

    if (vocbase != NULL) {
      if (TRI_EqualString(name, vocbase->_name)) {
        // name already in use
        TRI_ReadUnlockReadWriteLock(&server->_lock);
        TRI_UnlockMutex(&server->_createLock);

        return TRI_ERROR_ARANGO_DATABASE_NAME_USED;
      }
    }
  }

  // name not yet in use, release the read lock
  TRI_ReadUnlockReadWriteLock(&server->_lock);


  // create the database directory
  res = CreateDatabaseDirectory(server, name, defaults, &file);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_UnlockMutex(&server->_createLock);

    return res;
  }

  path = TRI_Concatenate2File(server->_databasePath, file);
  TRI_FreeString(TRI_CORE_MEM_ZONE, file);

  vocbase = TRI_OpenVocBase(path, name, defaults, false);
  TRI_FreeString(TRI_CORE_MEM_ZONE, path);

  if (vocbase == NULL) {
    TRI_UnlockMutex(&server->_createLock);

    // grab last error
    res = TRI_errno();

    if (res != TRI_ERROR_NO_ERROR) {
      // but we must have an error...
      res = TRI_ERROR_INTERNAL;
    }

    LOG_ERROR("could not create database '%s': %s", 
              name,
              TRI_errno_string(res));

    return res;
  }

  assert(vocbase != NULL);

  TRI_WriteLockReadWriteLock(&server->_lock);
  TRI_InsertKeyAssociativePointer(&server->_databases, vocbase->_name, vocbase, false);
  TRI_WriteUnlockReadWriteLock(&server->_lock);
  
  TRI_UnlockMutex(&server->_createLock);

  *database = vocbase;

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get a database by its name
////////////////////////////////////////////////////////////////////////////////

TRI_vocbase_t* TRI_GetDatabaseByNameServer (TRI_server_t* server,
                                            char const* name) {
  TRI_vocbase_t* vocbase;

  TRI_ReadLockReadWriteLock(&server->_lock);
  vocbase = TRI_LookupByKeyAssociativePointer(&server->_databases, name);
  TRI_ReadUnlockReadWriteLock(&server->_lock);

  return vocbase;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the list of all database names
////////////////////////////////////////////////////////////////////////////////

int TRI_GetDatabaseNamesServer (TRI_server_t* server,
                                TRI_vector_string_t* names) {

  size_t i, n;

  TRI_ReadLockReadWriteLock(&server->_lock);
  n = server->_databases._nrAlloc;

  for (i = 0; i < n; ++i) {
    TRI_vocbase_t* vocbase = server->_databases._table[i];

    if (vocbase != NULL) {
      TRI_PushBackVectorString(names, TRI_DuplicateStringZ(names->_memoryZone, vocbase->_name));
    }
  }
  TRI_ReadUnlockReadWriteLock(&server->_lock);

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief copies the defaults into the target
////////////////////////////////////////////////////////////////////////////////

void TRI_GetDatabaseDefaultsServer (TRI_server_t* server,
                                    TRI_vocbase_defaults_t* target) {
  // copy defaults into target
  memcpy(target, &server->_defaults, sizeof(TRI_vocbase_defaults_t));
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                    tick functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief create a new tick
////////////////////////////////////////////////////////////////////////////////

TRI_voc_tick_t TRI_NewTickServer () {
  uint64_t tick = ServerIdentifier;

  TRI_LockSpin(&TickLock);

  tick |= (++CurrentTick) << 16;

  TRI_UnlockSpin(&TickLock);

  return tick;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief updates the tick counter, with lock
////////////////////////////////////////////////////////////////////////////////

void TRI_UpdateTickServer (TRI_voc_tick_t tick) {
  TRI_LockSpin(&TickLock);
  UpdateTick(tick);
  TRI_UnlockSpin(&TickLock);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief updates the tick counter, without lock - only use at startup!!
////////////////////////////////////////////////////////////////////////////////

void TRI_FastUpdateTickServer (TRI_voc_tick_t tick) {
  UpdateTick(tick);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the current tick counter
////////////////////////////////////////////////////////////////////////////////

TRI_voc_tick_t TRI_CurrentTickServer () {
  TRI_voc_tick_t tick;

  TRI_LockSpin(&TickLock);
  tick = GetTick();
  TRI_UnlockSpin(&TickLock);

  return tick;
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                   other functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief msyncs a memory block between begin (incl) and end (excl)
////////////////////////////////////////////////////////////////////////////////

bool TRI_MSync (int fd, 
                void* mmHandle, 
                char const* begin, 
                char const* end) {
  uintptr_t p = (intptr_t) begin;
  uintptr_t q = (intptr_t) end;
  uintptr_t g = (intptr_t) PageSize;

  char* b = (char*)( (p / g) * g );
  char* e = (char*)( ((q + g - 1) / g) * g );
  int res;

  res = TRI_FlushMMFile(fd, &mmHandle, b, e - b, MS_SYNC);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_set_errno(res);

    return false;
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End: