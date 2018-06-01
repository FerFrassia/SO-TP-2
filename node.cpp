#include "node.h"
#include "picosha2.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <cstdlib>
#include <queue>
#include <atomic>
#include <mpi.h>
#include <map>
#include <semaphore.h>
#include <mutex>

#define ANSI_COLOR_RED     "\x1b[31m"

int total_nodes, mpi_rank;
Block *last_block_in_chain;
map<string,Block> node_blocks;

mutex lastBlockMtx;
mutex mutexChangeBlock;

MPI_Request* request = new MPI_Request;

//Cuando me llega una cadena adelantada, y tengo que pedir los bloques que me faltan
//Si nos separan más de VALIDATION_BLOCKS bloques de distancia entre las cadenas, se descarta por seguridad
bool verificar_y_migrar_cadena(const Block *rBlock, const MPI_Status *status){
  Block* blockchain = new Block[VALIDATION_BLOCKS];

  if (rBlock->index - last_block_in_chain->index >  VALIDATION_BLOCKS) {
      // por seguridad no migro de cadena
        delete []blockchain;
        mutexChangeBlock.unlock();

        return false;
  }

  //TODO: Enviar mensaje TAG_CHAIN_HASH
  char last_hash[HASH_SIZE];
  printf("\033[22;34m[%d] Mando mensaje de cambio de cadena al nodo %d \033[0m \n", mpi_rank, status->MPI_SOURCE);
  strncpy(last_hash, rBlock->block_hash,HASH_SIZE);
  MPI_Send(last_hash, HASH_SIZE, MPI_CHAR, status->MPI_SOURCE, TAG_CHAIN_HASH, MPI_COMM_WORLD /*,request*/); 

  //TODO: Recibir mensaje TAG_CHAIN_RESPONSE
  MPI_Status mistatus;
  int amount_blocks_received;
  MPI_Probe(status->MPI_SOURCE, TAG_CHAIN_RESPONSE, MPI_COMM_WORLD, &mistatus);
  MPI_Get_count(&mistatus, *MPI_BLOCK, &amount_blocks_received);
  printf("\x1b[32m[%d] El nodo %d me mando %d bloques \033[0m \n", mpi_rank, status->MPI_SOURCE, amount_blocks_received);
  
  MPI_Recv(blockchain, amount_blocks_received, *MPI_BLOCK, mistatus.MPI_SOURCE, TAG_CHAIN_RESPONSE, MPI_COMM_WORLD, &mistatus);
  printf("\x1b[32m[%d] Recibí la cadena que pedí, del nodo %d \033[0m \n", mpi_rank, status->MPI_SOURCE);
  
  //TODO: Verificar que los bloques recibidos
  //sean válidos y se puedan acoplar a la cadena
  string hash_hex_str;
  block_to_hash(&(blockchain[0]),hash_hex_str);
  if(blockchain[0].index !=rBlock->index || strncmp(hash_hex_str.c_str(),last_hash,HASH_SIZE)!=0){
    delete []blockchain;
    mutexChangeBlock.unlock();
    return false;
  }



  for (int i =1; i<amount_blocks_received; ++i){
    block_to_hash(&blockchain[i],hash_hex_str);
    if(strcmp(hash_hex_str.c_str(),blockchain[i].block_hash)!=0){
      delete []blockchain;
      mutexChangeBlock.unlock();
      return false;
    }

    if(blockchain[i-1].index-1!=blockchain[i].index || strncmp(blockchain[i-1].previous_block_hash, blockchain[i].block_hash, HASH_SIZE)!=0){
        delete []blockchain;
        mutexChangeBlock.unlock();
        return false;
    }

    if(node_blocks.find(hash_hex_str) != node_blocks.end() || blockchain[i].index ==1){
      
      for(int j=0;j<=i;++j){
        node_blocks[blockchain[j].block_hash]=blockchain[j];
      }

      last_block_in_chain = &node_blocks[blockchain[0].block_hash];
      delete []blockchain;
      mutexChangeBlock.unlock();
      return true;
    }

  }
  


  delete []blockchain;
   mutexChangeBlock.unlock();
  return false;
}

void agregar_como_ultimo(Block* b) {
  strcpy(b->previous_block_hash, last_block_in_chain->block_hash);
  last_block_in_chain = b;  
	lastBlockMtx.unlock();//aca iria el unlock <-- creo que esto no va TOCHECK: aca se hace unlock pero cunando salis de este metodo se vuelve a hacer unlock
}

//Verifica que el bloque tenga que ser incluido en la cadena, y lo agrega si corresponde
bool validate_block_for_chain(Block *rBlock, const MPI_Status *status){

  if(valid_new_block(rBlock)){

	lastBlockMtx.lock();//aca pondria el lock, porque lo critico me parece que es el mapa, el puntero y el chequeo de condiciones en este caso(el chequeo de condiciones va a cambiar si se agrego un nodo minado o no), por lo tanto el unlock iria en la funcion agregar_como_ultimo

    //Agrego el bloque al diccionario, aunque no
    //necesariamente eso lo agrega a la cadena
;
    node_blocks[string(rBlock->block_hash)] = *rBlock;

    //TODO: Si el índice del bloque recibido es 1
    //y mí último bloque actual tiene índice 0,
    //entonces lo agrego como nuevo último.
    if (rBlock->index == 1 && last_block_in_chain->index == 0) {
      agregar_como_ultimo(rBlock);
      printf("[%d] Agregué el bloque con index %d, enviado por %d, a la lista \n", mpi_rank, rBlock->index, status->MPI_SOURCE);
      return true;
    }

    //TODO: Si el índice del bloque recibido es
    //el siguiente a mí último bloque actual,
    //y el bloque anterior apuntado por el recibido es mí último actual,
    //entonces lo agrego como nuevo último.
    if (rBlock->index == last_block_in_chain->index + 1 && strcmp(rBlock->previous_block_hash, last_block_in_chain->block_hash) == 0) {
      agregar_como_ultimo(rBlock);
      printf("[%d] Agregué el bloque con index %d, enviado por %d, a la lista \n", mpi_rank, rBlock->index, status->MPI_SOURCE);
      return true;
    }
	 lastBlockMtx.unlock();//aca iría unlock porque acá se terminan los chequeos que efectivamente agregan bloques, y si llego acá es porque aun no retorné


    //TODO: Si el índice del bloque recibido es
    //el siguiente a mí último bloque actual,
    //pero el bloque anterior apuntado por el recibido no es mí último actual,
    //entonces hay una blockchain más larga que la mía.
   mutexChangeBlock.lock();

    if (rBlock->index == last_block_in_chain->index + 1 && strcmp(rBlock->previous_block_hash, last_block_in_chain->block_hash) != 0) {
      printf("[%d] Perdí la carrera por uno (%d) contra %d \n", mpi_rank, rBlock->index, status->MPI_SOURCE);
      return verificar_y_migrar_cadena(rBlock,status);
    }

    //TODO: Si el índice del bloque recibido es igual al índice de mi último bloque actual,
    //entonces hay dos posibles forks de la blockchain pero mantengo la mía
    if (rBlock->index == last_block_in_chain->index) {
      printf("[%d] Conflicto suave: Conflicto de branch (%d) contra %d \n",mpi_rank,rBlock->index,status->MPI_SOURCE);
      mutexChangeBlock.unlock();
      return false;
    }

    //TODO: Si el índice del bloque recibido es anterior al índice de mi último bloque actual,
    //entonces lo descarto porque asumo que mi cadena es la que está quedando preservada.
    if (rBlock->index < last_block_in_chain->index) {
      printf("[%d] Conflicto suave: Descarto el bloque (%d vs %d) contra %d \n",mpi_rank,rBlock->index,last_block_in_chain->index, status->MPI_SOURCE);
      mutexChangeBlock.unlock();
      return false;
    }

    //TODO: Si el índice del bloque recibido está más de una posición adelantada a mi último bloque actual,
    //entonces me conviene abandonar mi blockchain actual
    if (rBlock->index > last_block_in_chain->index + 1) {
      printf("[%d] Perdí la carrera por varios contra %d \n", mpi_rank, status->MPI_SOURCE);
      return verificar_y_migrar_cadena(rBlock,status);
    }
    mutexChangeBlock.unlock();
  }

  printf("[%d] Error duro: Descarto el bloque recibido de %d porque no es válido \n",mpi_rank,status->MPI_SOURCE);
  return false;
}

//Envia el bloque minado a todos los nodos
void broadcast_block(const Block *block){
  for(int i = (mpi_rank + 1) % total_nodes; i != mpi_rank; i = (i + 1) % total_nodes){
    printf("[%d] Enviando bloque con index %d a nodo %d \n", mpi_rank, block->index, i);
    MPI_Send(block, 1, *MPI_BLOCK, i, TAG_NEW_BLOCK, MPI_COMM_WORLD/*,request*/); 
  }
}

//Proof of work
//TODO: Advertencia: puede tener condiciones de carrera
void* proof_of_work(void *sem){

    string hash_hex_str;
    Block block;
    unsigned int mined_blocks = 0;
    while(true){

      if(last_block_in_chain->index>=300){
        printf("[%d] Mi ultimo bloque es el: %d, con hash: %s \n", mpi_rank, last_block_in_chain->index, last_block_in_chain->block_hash);
        //while(1);
        MPI_Abort(MPI_COMM_WORLD,3);
      }

      block = *last_block_in_chain;

      //Preparar nuevo bloque
      block.index += 1;
      block.node_owner_number = mpi_rank;
      block.difficulty = DEFAULT_DIFFICULTY;
      block.created_at = static_cast<unsigned long int> (time(NULL));
      memcpy(block.previous_block_hash,block.block_hash,HASH_SIZE);

      //Agregar un nonce al azar al bloque para intentar resolver el problema
      gen_random_nonce(block.nonce);

      //Hashear el contenido (con el nuevo nonce)
      block_to_hash(&block,hash_hex_str);

      //Contar la cantidad de ceros iniciales (con el nuevo nonce)
      if(solves_problem(hash_hex_str)){
      lastBlockMtx.lock();//acá pondría un lock, lo critico seria modificar el mapa y el puntero en esta funcion
    //Movemos el lock aca por que asi no se modifica 2 veces el last block in chain  cuando me broadcastearon un nuevo bloque 
          //Verifico que no haya cambiado mientras calcula
    //Aca va mutex que evita el camgio de last block indevido (mutexChangeBlock)

      mutexChangeBlock.lock();
          if(last_block_in_chain->index < block.index){
            mined_blocks += 1;

            /////CAMBIO EL CODIGO DE ELLOS, CREO QUE HACE FALTA ESTO
            // block.previous_block_hash[HASH_SIZE] = last_block_in_chain->block_hash[HASH_SIZE];
            strncpy(block.previous_block_hash, last_block_in_chain->block_hash,HASH_SIZE);
            /////FIN CAMBIO
            
            *last_block_in_chain = block;
            
            strncpy(last_block_in_chain->block_hash, hash_hex_str.c_str(),HASH_SIZE);
            node_blocks[hash_hex_str] = *last_block_in_chain;
            printf("[%d] Miné el bloque con index %d \n", mpi_rank, last_block_in_chain->index);
          
            //TODO: Mientras comunico, no responder mensajes de nuevos nodos
            sem_wait((sem_t*) sem);
            broadcast_block(last_block_in_chain);
            sem_post((sem_t*) sem);
          }
          mutexChangeBlock.unlock();
          lastBlockMtx.unlock();
      }

    }

    return NULL;
}


int node(){

  //Tomar valor de mpi_rank y de nodos totales
  MPI_Comm_size(MPI_COMM_WORLD, &total_nodes);
  MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);

  //La semilla de las funciones aleatorias depende del mpi_ranking
  srand(time(NULL) + mpi_rank);
  
  printf("[MPI] Lanzando proceso %u\n", mpi_rank);

  last_block_in_chain = new Block;
 
  //Inicializo el primer bloque
  last_block_in_chain->index = 0;
  last_block_in_chain->node_owner_number = mpi_rank;
  last_block_in_chain->difficulty = DEFAULT_DIFFICULTY;
  last_block_in_chain->created_at = static_cast<unsigned long int> (time(NULL));
  memset(last_block_in_chain->previous_block_hash,0,HASH_SIZE);

  //TODO: Crear thread para minar
  sem_t* sem_broadcast = new sem_t();
  sem_init(sem_broadcast, 0, 1);

  pthread_t thread;
  pthread_create(&thread, NULL, proof_of_work, (void*) sem_broadcast);

  MPI_Status status;
  int amount_blocks_received = 0;
  int amount_hash_received = 0;
  while(true){


      //TODO: Recibir mensajes de otros nodos
      MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

      //TODO: Si es un mensaje de nuevo bloque, llamar a la función
      // validate_block_for_chain con el bloque recibido y el estado de MPI
	if(status.MPI_TAG == TAG_NEW_BLOCK){
      MPI_Get_count(&status, *MPI_BLOCK, &amount_blocks_received);
        Block* block_received = new Block;
	     
        //para no escuchar mensajes de nuevos bloques mientras se está broadcasteando
        sem_wait(sem_broadcast);
        MPI_Recv(block_received , amount_blocks_received, *MPI_BLOCK, MPI_ANY_SOURCE, TAG_NEW_BLOCK, MPI_COMM_WORLD, &status);
	      sem_post(sem_broadcast);//la duda seria si el post va aca o mas abajo
        printf("[%d] Recibí el bloque con index %d, del nodo %d \n", mpi_rank, block_received->index, status.MPI_SOURCE);
        
        //Aca va el mutex que evita el cambio de last block (mutexChangeBlock)
        
        validate_block_for_chain(block_received, &status);
        
        amount_blocks_received = 0;
 
	}

      //TODO: Si es un mensaje de pedido de cadena,
      //responderlo enviando los bloques correspondientes
      MPI_Get_count(&status, MPI_CHAR, &amount_hash_received);
      if ( status.MPI_TAG == TAG_CHAIN_HASH) {
        char hash_buffer[HASH_SIZE];
        MPI_Recv(hash_buffer, amount_hash_received, MPI_CHAR, MPI_ANY_SOURCE, TAG_CHAIN_HASH, MPI_COMM_WORLD, &status);
        printf("[%d] Recibí pedido de cambio de cadena del nodo %d \n", mpi_rank, status.MPI_SOURCE);

        Block* blockchain = new Block[VALIDATION_BLOCKS];
        Block bloque = node_blocks[std::string(hash_buffer)];
        int i;

        for(i =0; i< VALIDATION_BLOCKS && bloque.index>1 ;++i){
          blockchain[i] = bloque;
           printf("\x1b[33m[%d] Agrego el bloque %d \033[0m \n", mpi_rank, bloque.index);
          bloque=node_blocks[string(bloque.previous_block_hash)];
        }

        if(bloque.index ==1){
          blockchain[i]= bloque;
        }

        MPI_Send(blockchain, i, *MPI_BLOCK, status.MPI_SOURCE, TAG_CHAIN_RESPONSE, MPI_COMM_WORLD/*,request*/); 
        printf("\x1b[33m[%d] Envío cadena pedida por el nodo %d, de tamano %d \033[0m \n", mpi_rank, status.MPI_SOURCE, i);
      
        amount_hash_received = 0;
        if(mpi_rank==1)
        printf("\x1b[33m[%d] Borro el buffer de bloques \033[0m \n", mpi_rank);
        delete []blockchain;
      if(mpi_rank==1)
        printf("\x1b[33m[%d] Ya borre el buffer de bloques \033[0m \n", mpi_rank);


      }
  }

  delete last_block_in_chain;
  return 0;
}
