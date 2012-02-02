////////////////////////////////////////////////////////////////////////////////
/// @brief AST query declarations
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2012 triagens GmbH, Cologne, Germany
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
/// @author Copyright 2012, triagens GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "ast-query.h"


// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup QL
/// @{
////////////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////////
/// @brief Hash function used to hash collection aliases
////////////////////////////////////////////////////////////////////////////////

static uint64_t HashKey (TRI_associative_pointer_t* array, void const* key) {
  char const* k = (char const*) key;

  return TRI_FnvHashString(k);
}


////////////////////////////////////////////////////////////////////////////////
/// @brief Hash function used to hash elements in the collection
////////////////////////////////////////////////////////////////////////////////

static uint64_t HashElement (TRI_associative_pointer_t* array, void const* element) {
  QL_ast_query_collection_t *collection = (QL_ast_query_collection_t*) element;

  return TRI_FnvHashString(collection->_alias);
}


////////////////////////////////////////////////////////////////////////////////
/// @brief Comparison function used to determine hash key equality
////////////////////////////////////////////////////////////////////////////////

static bool EqualKeyElement (TRI_associative_pointer_t* array, void const* key, void const* element) {
  char const* k = (char const*) key;
  QL_ast_query_collection_t *collection = (QL_ast_query_collection_t*) element;

  return TRI_EqualString(k, collection->_alias);
}


////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////


// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup QL
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief Initialize data structures for a query
////////////////////////////////////////////////////////////////////////////////

void QLAstQueryInit (QL_ast_query_t *query) {
  query->_type = QLQueryTypeUndefined;
  query->_from._base     = 0;
  query->_where._base    = 0;
  query->_order._base    = 0;
  query->_limit._isUsed  = false;

  TRI_InitAssociativePointer(&query->_from._collections,
                             HashKey,
                             HashElement,
                             EqualKeyElement,
                             0);
}


////////////////////////////////////////////////////////////////////////////////
/// @brief De-allocate data structures for a query
////////////////////////////////////////////////////////////////////////////////

void QLAstQueryFree (QL_ast_query_t *query) {
  size_t i; 
  void *ptr;

  // destroy all elements in collection array
  for (i = 0; i < query->_from._collections._nrAlloc; i++) {
    ptr = query->_from._collections._table[i];

    if (ptr != 0) {
      TRI_Free(ptr);
    }
  }

  // destroy array itself
  TRI_DestroyAssociativePointer(&query->_from._collections);
}


////////////////////////////////////////////////////////////////////////////////
/// @brief Check if a collection was defined in a query
////////////////////////////////////////////////////////////////////////////////

bool QLAstQueryIsValidAlias (QL_ast_query_t *query, const char *alias) {
  return (0 != TRI_LookupByKeyAssociativePointer (&query->_from._collections, alias));
}


////////////////////////////////////////////////////////////////////////////////
/// @brief Add a collection to the query
////////////////////////////////////////////////////////////////////////////////

bool QLAstQueryAddCollection (QL_ast_query_t *query, const char *name, const char *alias, const bool isPrimary) {
  QL_ast_query_collection_t *collection;
  QL_ast_query_collection_t *previous;

  previous =  (QL_ast_query_collection_t *) TRI_LookupByKeyAssociativePointer (&query->_from._collections, alias);

  if (previous != 0) {
    // element is already present - return an error
    return false;
  }

  collection = (QL_ast_query_collection_t *) TRI_Allocate(sizeof(QL_ast_query_collection_t));
  if (collection == 0) { 
    return false;
  }

  collection->_name      = (char *) name;
  collection->_alias     = (char *) alias;
  collection->_isPrimary = isPrimary;

  TRI_InsertKeyAssociativePointer(&query->_from._collections, collection->_alias, collection, true);

  return true;
}


////////////////////////////////////////////////////////////////////////////////
/// @brief get the name of the primary collection used in the query
////////////////////////////////////////////////////////////////////////////////

char *QLAstQueryGetPrimaryName (const QL_ast_query_t *query) {
  size_t i;
  QL_ast_query_collection_t *collection;

  for (i = 0; i < query->_from._collections._nrAlloc; i++) {
    collection = query->_from._collections._table[i];
    if (collection != 0 && collection->_isPrimary) {
      return collection->_name;
    }
  }
  
  return 0;
}


////////////////////////////////////////////////////////////////////////////////
/// @brief get the alias of the primary collection used in the query
////////////////////////////////////////////////////////////////////////////////
char *QLAstQueryGetPrimaryAlias (const QL_ast_query_t *query) {
  size_t i;
  QL_ast_query_collection_t *collection;

  for (i = 0; i < query->_from._collections._nrAlloc; i++) {
    collection = query->_from._collections._table[i];
    if (collection != 0 && collection->_isPrimary) {
      return collection->_alias;
    }
  }
  
  return 0;
}


////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////


// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|// --SECTION--\\|/// @\\}\\)"
// End:

