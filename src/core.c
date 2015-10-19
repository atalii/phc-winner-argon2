/*
 * Argon2 source code package
 *
 * Written by Daniel Dinu and Dmitry Khovratovich, 2015
 *
 * This work is licensed under a Creative Commons CC0 1.0 License/Waiver.
 *
 * You should have received a copy of the CC0 Public Domain Dedication along with
 * this software. If not, see <http://creativecommons.org/publicdomain/zero/1.0/>.
 */


/*For memory wiping*/
#ifdef _MSC_VER
#include "windows.h"
#include "winbase.h" //For SecureZeroMemory
#endif
#if defined __STDC_LIB_EXT1__
#define __STDC_WANT_LIB_EXT1__ 1
#endif
#define VC_GE_2005( version )       ( version >= 1400 )


#include <inttypes.h>
#ifndef _MSC_VER
#include <pthread.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "argon2.h"
#include "core.h"
#include "kat.h"


#include "blake2/blake2.h"
#include "blake2/blake2-impl.h"

#if defined(__clang__)
#if __has_attribute(optnone)
#define NOT_OPTIMIZED __attribute__((optnone))
#endif
#elif defined(__GNUC__)
#define GCC_VERSION (__GNUC__ * 10000 \
                    + __GNUC_MINOR__ * 100 \
                    + __GNUC_PATCHLEVEL__)
#if GCC_VERSION >= 40400
#define NOT_OPTIMIZED __attribute__((optimize("O0")))
#endif
#endif
#ifndef NOT_OPTIMIZED
#define NOT_OPTIMIZED
#endif

/***************Instance and Position constructors**********/
void init_block_value( block *b, uint8_t in )
{
    memset( b->v,in,sizeof( b->v ) );
}

void copy_block( block *dst, const block *src )
{
    memcpy( dst->v,src->v,sizeof( uint64_t )*ARGON2_WORDS_IN_BLOCK );
}

void xor_block( block *dst, const  block *src )
{
    int i;

    for( i=0; i<ARGON2_WORDS_IN_BLOCK; ++i )
    {
        dst->v[i] ^= src->v[i];
    }
}


/***************Memory allocators*****************/
int allocate_memory( block **memory, uint32_t m_cost )
{
    if ( memory != NULL )
    {
        *memory = ( block * )malloc( sizeof( block )*m_cost );

        if ( !*memory )
        {
            return ARGON2_MEMORY_ALLOCATION_ERROR;
        }

        return ARGON2_OK;
    }
    else return ARGON2_MEMORY_ALLOCATION_ERROR;
}

/* Function that securely cleans the memory
 * @param mem Pointer to the memory
 * @param s Memory size in bytes
 */

static __inline void NOT_OPTIMIZED secure_wipe_memory( void *v, size_t n )
{
#if defined  (_MSC_VER ) &&  VC_GE_2005( _MSC_VER )
    SecureZeroMemory( v, n );
#elif defined memset_s
    memset_s( v, n );
#elif defined( __OpenBSD__ )
    explicit_bzero( memory, size );
#else
    static void *( *const volatile memset_sec )( void *, int, size_t ) = &memset;
    memset_sec( v, 0, n );
#endif
}

/*************************Argon2 internal constants**************************************************/

/* Version of the algorithm */
const uint32_t ARGON2_VERSION_NUMBER = 0x10;

/* Memory block size in bytes */
#define  ARGON2_BLOCK_SIZE  1024
#define ARGON2_WORDS_IN_BLOCK (ARGON2_BLOCK_SIZE/8)
const uint32_t ARGON2_QWORDS_IN_BLOCK = 64; /*Dependent values!*/

/* Number of pseudo-random values generated by one call to Blake in Argon2i  to generate reference block positions*/
const uint32_t ARGON2_ADDRESSES_IN_BLOCK = 128;



/*********Memory functions*/

void clear_memory( Argon2_instance_t *instance, bool clear )
{
    if ( instance->memory != NULL && clear )
    {
        secure_wipe_memory( instance->memory, sizeof ( block ) * instance->memory_blocks );
    }
}

void free_memory( block *memory )
{
    if ( memory != NULL )
    {
        free( memory );
    }
}

void finalize( const Argon2_Context *context, Argon2_instance_t *instance )
{
    if ( context != NULL && instance != NULL )
    {
        block blockhash;
        copy_block( &blockhash, instance->memory+ instance->lane_length - 1 );

        // XOR the last blocks
        for ( uint32_t l = 1; l < instance->lanes; ++l )
        {
            uint32_t last_block_in_lane = l * instance->lane_length + ( instance->lane_length - 1 );
            xor_block( &blockhash,instance->memory + last_block_in_lane );

        }

        // Hash the result
        blake2b_long( context->out, ( uint8_t * ) blockhash.v, context->outlen, ARGON2_BLOCK_SIZE );
        secure_wipe_memory( blockhash.v, ARGON2_BLOCK_SIZE ); //clear the blockhash

        if( context->print ) //Shall we print the output tag?
        {
            print_tag( context->out, context->outlen );
        }

        // Clear memory
        clear_memory( instance, context->clear_memory );

        // Deallocate the memory
        if ( NULL != context->free_cbk )
        {
            context->free_cbk( ( uint8_t * ) instance->memory, instance->memory_blocks * sizeof ( block ) );
        }
        else
        {
            free_memory( instance->memory );
        }

    }
}

uint32_t index_alpha( const Argon2_instance_t *instance, const Argon2_position_t *position, uint32_t pseudo_rand, bool same_lane )
{
    /*
     * Pass 0:
     *      This lane : all already finished segments plus already constructed blocks in this segment
     *      Other lanes : all already finished segments
     * Pass 1+:
     *      This lane : (SYNC_POINTS - 1) last segments plus already constructed blocks in this segment
     *      Other lanes : (SYNC_POINTS - 1) last segments
     */
    uint32_t reference_area_size;

    if ( 0 == position->pass )
    {
        // First pass
        if ( 0 == position->slice )
        {
            // First slice
            reference_area_size = position->index - 1; // all but the previous
        }
        else
        {
            if ( same_lane )
            {
                // The same lane => add current segment
                reference_area_size = position->slice * instance->segment_length + position->index - 1;
            }
            else
            {
                reference_area_size = position->slice * instance->segment_length + ( ( position->index == 0 ) ? ( -1 ) : 0 );
            }
        }
    }
    else
    {
        // Second pass
        if ( same_lane )
        {
            reference_area_size = instance->lane_length - instance->segment_length + position->index - 1;
        }
        else
        {
            reference_area_size = instance->lane_length - instance->segment_length + ( ( position->index == 0 ) ? ( -1 ) : 0 );
        }
    }

    /* 1.2.4. Mapping pseudo_rand to 0..<reference_area_size-1> and produce relative position */
    uint64_t relative_position = pseudo_rand;
    relative_position = relative_position * relative_position >> 32;
    relative_position = reference_area_size - 1 - ( reference_area_size * relative_position >> 32 );

    /* 1.2.5 Computing starting position */
    uint32_t start_position = 0;

    if ( 0 != position->pass )
    {
        start_position = ( position->slice == ARGON2_SYNC_POINTS - 1 ) ? 0 : ( position->slice + 1 ) * instance->segment_length;
    }

    /* 1.2.6. Computing absolute position */
    uint32_t absolute_position = ( start_position + relative_position ) % instance->lane_length; // absolute position
    return absolute_position;
}

void fill_memory_blocks( Argon2_instance_t *instance )
{
    if ( instance == NULL )
    {
        return;
    }

    for ( uint32_t r = 0; r < instance->passes; ++r )
    {
        for ( uint8_t s = 0; s < ARGON2_SYNC_POINTS; ++s )
        {
#ifndef _MSC_VER
            //1. Allocating space for threads
            pthread_t *thread = malloc( sizeof( pthread_t )*( instance->lanes ) );
            Argon2_thread_data *thr_data = malloc( sizeof( Argon2_thread_data )*( instance->lanes ) );
            pthread_attr_t attr;
            int rc;
            void *status;
            pthread_attr_init( &attr );
            pthread_attr_setdetachstate( &attr,PTHREAD_CREATE_JOINABLE );

            //2. Calling threads
            for ( uint32_t l = 0; l < instance->lanes; ++l )
            {
                //2.1 Join a thread if limit is exceeded
                if( l>=instance->threads )
                {
                    rc = pthread_join( thread[l-instance->threads],&status );

                    if ( rc )
                    {
                        printf( "ERROR; return code from pthread_join() is %d\n", rc );
                        exit( -1 );
                    }
                }

                //2.2 Create thread
                Argon2_position_t position = {r,l,s,0};
                thr_data[l].instance_ptr = instance;//preparing the thread input
                memcpy( &( thr_data[l].pos ), &position, sizeof( Argon2_position_t ) );
                rc = pthread_create( &thread[l],&attr,fill_segment_thr,( void * )&thr_data[l] );

                //FillSegment(instance, position);  //Non-thread equivalent of the lines above
            }

            //3. Joining remaining threads
            for ( uint32_t l = instance->lanes - instance->threads; l < instance->lanes; ++l )
            {
                rc = pthread_join( thread[l],&status );

                if ( rc )
                {
                    printf( "ERROR; return code from pthread_join() is %d\n", rc );
                    exit( -1 );
                }
            }

            free( thread );
            pthread_attr_destroy( &attr );
            free( thr_data );
#else   //No threads for Windows
			for (uint32_t l = 0; l < instance->lanes; ++l)
			{
				Argon2_position_t position = { r, l, s, 0 };
				fill_segment(instance, position);
			}
#endif
        }

        if( instance->print_internals )
        {
            internal_kat( instance, r ); // Print all memory blocks
        }
    }


}

int validate_inputs( const Argon2_Context *context )
{
    if ( NULL == context )
    {
        return ARGON2_INCORRECT_PARAMETER;
    }

    if ( NULL == context->out )
    {
        return ARGON2_OUTPUT_PTR_NULL;
    }

    /* Validate output length */
    if ( ARGON2_MIN_OUTLEN > context->outlen )
    {
        return ARGON2_OUTPUT_TOO_SHORT;
    }

    if ( ARGON2_MAX_OUTLEN < context->outlen )
    {
        return ARGON2_OUTPUT_TOO_LONG;
    }

    /* Validate password length */
    if ( NULL == context->pwd )
    {
        if ( 0 != context->pwdlen )
        {
            return ARGON2_PWD_PTR_MISMATCH;
        }
    }
    else
    {
        if ( ARGON2_MIN_PWD_LENGTH != 0 && ARGON2_MIN_PWD_LENGTH > context->pwdlen )
        {
            return ARGON2_PWD_TOO_SHORT;
        }

        if ( ARGON2_MAX_PWD_LENGTH < context->pwdlen )
        {
            return ARGON2_PWD_TOO_LONG;
        }
    }

    /* Validate salt length */
    if ( NULL == context->salt )
    {
        if ( 0 != context->saltlen )
        {
            return ARGON2_SALT_PTR_MISMATCH;
        }
    }
    else
    {
        if ( ARGON2_MIN_SALT_LENGTH > context->saltlen )
        {
            return ARGON2_SALT_TOO_SHORT;
        }

        if ( ARGON2_MAX_SALT_LENGTH < context->saltlen )
        {
            return ARGON2_SALT_TOO_LONG;
        }
    }

    /* Validate secret length */
    if ( NULL == context->secret )
    {
        if ( 0 != context->secretlen )
        {
            return ARGON2_SECRET_PTR_MISMATCH;
        }
    }
    else
    {
        if ( ARGON2_MIN_SECRET > context->secretlen )
        {
            return ARGON2_SECRET_TOO_SHORT;
        }

        if ( ARGON2_MAX_SECRET < context->secretlen )
        {
            return ARGON2_SECRET_TOO_LONG;
        }
    }

    /* Validate associated data */
    if ( NULL == context->ad )
    {
        if ( 0 != context->adlen )
        {
            return ARGON2_AD_PTR_MISMATCH;
        }
    }
    else
    {
        if ( ARGON2_MIN_AD_LENGTH > context->adlen )
        {
            return ARGON2_AD_TOO_SHORT;
        }

        if ( ARGON2_MAX_AD_LENGTH < context->adlen )
        {
            return ARGON2_AD_TOO_LONG;
        }
    }

    /* Validate memory cost */
    if ( ARGON2_MIN_MEMORY > context->m_cost )
    {
        return ARGON2_MEMORY_TOO_LITTLE;
    }

    if ( ARGON2_MAX_MEMORY < context->m_cost )
    {
        return ARGON2_MEMORY_TOO_MUCH;
    }

    /* Validate time cost */
    if ( ARGON2_MIN_TIME > context->t_cost )
    {
        return ARGON2_TIME_TOO_SMALL;
    }

    if ( ARGON2_MAX_TIME < context->t_cost )
    {
        return ARGON2_TIME_TOO_LARGE;
    }

    /* Validate lanes */
    if ( ARGON2_MIN_LANES > context->lanes )
    {
        return ARGON2_LANES_TOO_FEW;
    }

    if ( ARGON2_MAX_LANES < context->lanes )
    {
        return ARGON2_LANES_TOO_MANY;
    }

    /* Validate threads */
    if ( ARGON2_MIN_THREADS > context->threads )
    {
        return ARGON2_THREADS_TOO_FEW;
    }

    if ( ARGON2_MAX_THREADS < context->threads )
    {
        return ARGON2_THREADS_TOO_MANY;
    }

    if ( NULL != context->allocate_cbk && NULL == context->free_cbk )
    {
        return ARGON2_FREE_MEMORY_CBK_NULL;
    }

    if ( NULL == context->allocate_cbk && NULL != context->free_cbk )
    {
        return ARGON2_ALLOCATE_MEMORY_CBK_NULL;
    }

    return ARGON2_OK;
}

void fill_first_blocks( uint8_t *blockhash, const Argon2_instance_t *instance )
{
    // Make the first and second block in each lane as G(H0||i||0) or G(H0||i||1)
    for ( uint32_t l = 0; l < instance->lanes; ++l )
    {
        store32( blockhash+ARGON2_PREHASH_DIGEST_LENGTH,0 );
        store32( blockhash+ARGON2_PREHASH_DIGEST_LENGTH + 4,l );
        blake2b_long( ( uint8_t * ) ( instance->memory[l * instance->lane_length].v ), blockhash, ARGON2_BLOCK_SIZE, ARGON2_PREHASH_SEED_LENGTH );

        store32( blockhash+ARGON2_PREHASH_DIGEST_LENGTH,1 );
        blake2b_long( ( uint8_t * ) ( instance->memory[l * instance->lane_length + 1].v ), blockhash, ARGON2_BLOCK_SIZE, ARGON2_PREHASH_SEED_LENGTH );
    }
}

void initial_hash( uint8_t *blockhash, Argon2_Context *context, Argon2_type type )
{
    blake2b_state BlakeHash;
    uint8_t value[sizeof ( uint32_t )];

    if ( NULL == context || NULL == blockhash )
    {
        return;
    }

    blake2b_init( &BlakeHash, ARGON2_PREHASH_DIGEST_LENGTH );

    store32( &value, context->lanes );
    blake2b_update( &BlakeHash, ( const uint8_t * ) &value, sizeof ( value ) );

    store32( &value, context->outlen );
    blake2b_update( &BlakeHash, ( const uint8_t * ) &value, sizeof ( value ) );

    store32( &value, context->m_cost );
    blake2b_update( &BlakeHash, ( const uint8_t * ) &value, sizeof ( value ) );

    store32( &value, context->t_cost );
    blake2b_update( &BlakeHash, ( const uint8_t * ) &value, sizeof ( value ) );

    store32( &value, ARGON2_VERSION_NUMBER );
    blake2b_update( &BlakeHash, ( const uint8_t * ) &value, sizeof ( value ) );

    store32( &value, ( uint32_t ) type );
    blake2b_update( &BlakeHash, ( const uint8_t * ) &value, sizeof ( value ) );

    store32( &value, context->pwdlen );
    blake2b_update( &BlakeHash, ( const uint8_t * ) &value, sizeof ( value ) );

    if ( context->pwd != NULL )
    {
        blake2b_update( &BlakeHash, ( const uint8_t * ) context->pwd, context->pwdlen );

        if ( context->clear_password )
        {
            secure_wipe_memory( context->pwd, context->pwdlen );
            context->pwdlen = 0;
        }
    }

    store32( &value, context->saltlen );
    blake2b_update( &BlakeHash, ( const uint8_t * ) &value, sizeof ( value ) );

    if ( context->salt != NULL )
    {
        blake2b_update( &BlakeHash, ( const uint8_t * ) context->salt, context->saltlen );
    }

    store32( &value, context->secretlen );
    blake2b_update( &BlakeHash, ( const uint8_t * ) &value, sizeof ( value ) );

    if ( context->secret != NULL )
    {
        blake2b_update( &BlakeHash, ( const uint8_t * ) context->secret, context->secretlen );

        if ( context->clear_secret )
        {
            secure_wipe_memory( context->secret, context->secretlen );
            context->secretlen = 0;
        }
    }

    store32( &value, context->adlen );
    blake2b_update( &BlakeHash, ( const uint8_t * ) &value, sizeof ( value ) );

    if ( context->ad != NULL )
    {
        blake2b_update( &BlakeHash, ( const uint8_t * ) context->ad, context->adlen );
    }

    blake2b_final( &BlakeHash, blockhash, ARGON2_PREHASH_DIGEST_LENGTH );
}

int initialize( Argon2_instance_t *instance, Argon2_Context *context )
{
    if ( instance == NULL || context == NULL )
        return ARGON2_INCORRECT_PARAMETER;

    // 1. Memory allocation
    int result = ARGON2_OK;

    if ( NULL != context->allocate_cbk )
    {
        result = context->allocate_cbk( ( uint8_t ** )&( instance->memory ), instance->memory_blocks * ARGON2_BLOCK_SIZE );
    }
    else
    {
        result = allocate_memory( &( instance->memory ), instance->memory_blocks );
    }

    if ( ARGON2_OK != result )
    {
        return result;
    }

    // 2. Initial hashing
    // H_0 + 8 extra bytes to produce the first blocks
    uint8_t blockhash[ARGON2_PREHASH_SEED_LENGTH];
    // Hashing all inputs
    initial_hash( blockhash, context, instance->type );
    // Zeroing 8 extra bytes
    secure_wipe_memory( blockhash + ARGON2_PREHASH_DIGEST_LENGTH, ARGON2_PREHASH_SEED_LENGTH - ARGON2_PREHASH_DIGEST_LENGTH );

    if( context->print )
    {
        initial_kat( blockhash, context, instance->type );
    }

    // 3. Creating first blocks, we always have at least two blocks in a slice
    fill_first_blocks( blockhash, instance );
    // Clearing the hash
    secure_wipe_memory( blockhash, ARGON2_PREHASH_SEED_LENGTH );

    return ARGON2_OK;
}

int argon2_core( Argon2_Context *context, Argon2_type type )
{
    /* 1. Validate all inputs */
    int result = validate_inputs( context );

    if ( ARGON2_OK != result )
    {
        return result;
    }

    if ( Argon2_d != type && Argon2_i != type )
    {
        return ARGON2_INCORRECT_TYPE;
    }

    /* 2. Align memory size */
    // Minimum memory_blocks = 8L blocks, where L is the number of lanes
    uint32_t memory_blocks = context->m_cost;

    if ( memory_blocks < 2 * ARGON2_SYNC_POINTS * context->lanes )
    {
        memory_blocks = 2 * ARGON2_SYNC_POINTS * context->lanes;
    }

    uint32_t segment_length = memory_blocks / ( context->lanes * ARGON2_SYNC_POINTS );
    bool print_internals = context->print;
    // Ensure that all segments have equal length
    memory_blocks = segment_length * ( context->lanes * ARGON2_SYNC_POINTS );
    Argon2_instance_t instance = { NULL, context->t_cost, memory_blocks, segment_length,
                                   segment_length * ARGON2_SYNC_POINTS, context->lanes, context->threads, type, print_internals
                                 };

    /* 3. Initialization: Hashing inputs, allocating memory, filling first blocks */
    result = initialize( &instance, context );

    if ( ARGON2_OK != result )
    {
        return result;
    }

    /* 4. Filling memory */
    fill_memory_blocks( &instance );

    /* 5. Finalization */
    finalize( context, &instance );

    return ARGON2_OK;
}

#ifndef _MSC_VER  /* No threads for Windows*/
void *fill_segment_thr( void *thread_data )
{
    Argon2_thread_data *my_data = ( Argon2_thread_data * )thread_data;
    fill_segment( my_data->instance_ptr, my_data->pos );
    pthread_exit( thread_data );
}
#endif