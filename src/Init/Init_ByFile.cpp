#include "GAMER.h"

// declare as static so that other functions cannot invoke it directly and must use the function pointer
static void Init_ByFile_Default( real fluid_out[], const real fluid_in[], const int nvar_in,
                                 const double x, const double y, const double z, const double Time,
                                 const int lv, double AuxArray[] );

// this function pointer may be overwritten by various test problem initializers
void (*Init_ByFile_User_Ptr)( real fluid_out[], const real fluid_in[], const int nvar_in,
                              const double x, const double y, const double z, const double Time,
                              const int lv, double AuxArray[] ) = Init_ByFile_Default;

static void Init_ByFile_AssignData( const char UM_Filename[], const int UM_lv, const int UM_NVar, const int UM_LoadNRank );




//-------------------------------------------------------------------------------------------------------
// Function    :  Init_ByFile
// Description :  Set up initial condition from an input uniform-mesh array
//
// Note        :  1. Create levels from 0 to OPT__UM_IC_LEVEL
//                   --> No levels above OPT__UM_IC_LEVEL will be created unless OPT__UM_IC_REFINE is enabled,
//                       for which levels OPT__UM_IC_LEVEL+1 ~ MAX_LEVEL may also be generated based on the
//                       given refinement criteria
//                   --> ALL patches on levels 0 to OPT__UM_IC_LEVEL will be created. In other words,
//                       the simulation domain will be fully refined to level OPT__UM_IC_LEVEL.
//                       --> But if OPT__UM_IC_DOWNGRADE is enabled, patches on levels 1~OPT__UM_IC_LEVEL
//                           may be removed if not satisfying the refinement criteria
//                2. The uniform-mesh input file must be named as "UM_IC"
//                3. This function can load any number of input variables per cell (from 1 to NCOMP_TOTAL)
//                   --> Determined by the input parameter "OPT__UM_IC_NVAR"
//                   --> If "OPT__UM_IC_NVAR < NCOMP_TOTAL", one must specify the way to assign values to all
//                       variables using the function pointer Init_ByFile_User_Ptr()
//                4. The data format in the UM_IC file should be [k][j][i][v] instead of [v][k][j][i]
//                   --> Different from the data layout of patch->fluid
//                5. Does not work with rectangular domain decomposition anymore
//                   --> Must enable either SERIAL or LOAD_BALANCE
//                6. OpenMP is not supported yet
//
// Parameter   :  None
//-------------------------------------------------------------------------------------------------------
void Init_ByFile()
{

   if ( MPI_Rank == 0 )    Aux_Message( stdout, "%s ...\n", __FUNCTION__ );


   const char UM_Filename[] = "UM_IC";
   const int  UM_Size3D[3]  = { NX0_TOT[0]*(1<<OPT__UM_IC_LEVEL),
                                NX0_TOT[1]*(1<<OPT__UM_IC_LEVEL),
                                NX0_TOT[2]*(1<<OPT__UM_IC_LEVEL) };

// check
#  if ( !defined SERIAL  &&  !defined LOAD_BALANCE )
      Aux_Error( ERROR_INFO, "must enable either SERIAL or LOAD_BALANCE for %s !!\n", __FUNCTION__ );
#  endif

   if ( OPT__UM_IC_LEVEL < 0  ||  OPT__UM_IC_LEVEL > TOP_LEVEL )
      Aux_Error( ERROR_INFO, "OPT__UM_IC_LEVEL (%d) > TOP_LEVEL (%d) !!\n", OPT__UM_IC_LEVEL, TOP_LEVEL );

   if ( OPT__UM_IC_NVAR < 1  ||  OPT__UM_IC_NVAR > NCOMP_TOTAL )
      Aux_Error( ERROR_INFO, "invalid OPT__UM_IC_NVAR = %d (accepeted range: %d ~ %d) !!\n",
                 OPT__UM_IC_NVAR, 1, NCOMP_TOTAL );

   if ( !Aux_CheckFileExist(UM_Filename) )
      Aux_Error( ERROR_INFO, "file \"%s\" does not exist !!\n", UM_Filename );

// check file size
   FILE *FileTemp = fopen( UM_Filename, "rb" );

   fseek( FileTemp, 0, SEEK_END );

   const long ExpectSize = long(OPT__UM_IC_NVAR)*(long)UM_Size3D[0]*(long)UM_Size3D[1]*(long)UM_Size3D[2]*sizeof(real);
   const long FileSize   = ftell( FileTemp );
   if ( FileSize != ExpectSize )
      Aux_Error( ERROR_INFO, "size of the file <%s> (%ld) != expect (%ld) !!\n", UM_Filename, FileSize, ExpectSize );

   fclose( FileTemp );

   MPI_Barrier( MPI_COMM_WORLD );



// 1. allocate all real patches on levels 0 ~ OPT__UM_IC_LEVEL
   const bool FindHomePatchForPar_Yes = true;
   const bool FindHomePatchForPar_No  = false;

   for (int lv=0; lv<=OPT__UM_IC_LEVEL; lv++)
   {
      if ( MPI_Rank == 0 )    Aux_Message( stdout, "   Allocating level %d ...\n", lv );

//    associate particles with home patches on the level **OPT__UM_IC_LEVEL** only
      Init_UniformGrid( lv, (lv==OPT__UM_IC_LEVEL)?FindHomePatchForPar_Yes:FindHomePatchForPar_No );

//    construct IdxList_Real[]
//    --> necessary for calling LB_Init_LoadBalance() later with Redistribute_No
#     ifdef LOAD_BALANCE
      if ( amr->LB->IdxList_Real         [lv] != NULL )   delete [] amr->LB->IdxList_Real         [lv];
      if ( amr->LB->IdxList_Real_IdxTable[lv] != NULL )   delete [] amr->LB->IdxList_Real_IdxTable[lv];

      amr->LB->IdxList_Real         [lv] = new long [ amr->NPatchComma[lv][1] ];
      amr->LB->IdxList_Real_IdxTable[lv] = new int  [ amr->NPatchComma[lv][1] ];

      for (int PID=0; PID<amr->NPatchComma[lv][1]; PID++)
         amr->LB->IdxList_Real[lv][PID] = amr->patch[0][lv][PID]->LB_Idx;

      Mis_Heapsort( amr->NPatchComma[lv][1], amr->LB->IdxList_Real[lv], amr->LB->IdxList_Real_IdxTable[lv] );
#     endif

//    get the total number of real patches
      Mis_GetTotalPatchNumber( lv );

      if ( MPI_Rank == 0 )    Aux_Message( stdout, "   Allocating level %d ... done\n", lv );
   } // for (int lv=0; lv<=OPT__UM_IC_LEVEL; lv++)



// 2. initialize load-balancing (or construct patch relation for SERIAL)
#  ifdef LOAD_BALANCE
// no need to redistribute patches since Init_UniformGrid() already takes into account load balancing
// --> but particle weighting is not considered yet
// --> we will invoke LB_Init_LoadBalance() again after Flu_Restrict() for that

// must not reset load-balance variables (i.e., must adopt ResetLB_No) to avoid overwritting IdxList_Real[]
// and IdxList_Real_IdxList[] already set above
   const double ParWeight_Zero   = 0.0;
   const bool   Redistribute_Yes = true;
   const bool   Redistribute_No  = false;
   const bool   ResetLB_Yes      = true;
   const bool   ResetLB_No       = false;
   const int    AllLv            = -1;

   LB_Init_LoadBalance( Redistribute_No, ParWeight_Zero, ResetLB_No, AllLv );

#  else // for SERIAL

   for (int lv=0; lv<=OPT__UM_IC_LEVEL; lv++)
   {
//    set up BaseP[]
      if ( lv == 0 )
      Init_RecordBasePatch();

//    construct father-son relation
      if ( lv > 0 )
      FindFather( lv, 1 );

//    construct sibling relation
      SiblingSearch( lv );

//    allocate flux arrays on "lv-1"
//    --> must do this after constructing the patch relation on lv-1 and lv
      if ( lv > 0  &&  amr->WithFlux )
      Flu_AllocateFluxArray( lv-1 );
   }
#  endif // #ifdef LOAD_BALANCE ... else ...



// 3. assign data on level OPT__UM_IC_LEVEL by the input file UM_IC
   Init_ByFile_AssignData( UM_Filename, OPT__UM_IC_LEVEL, OPT__UM_IC_NVAR, OPT__UM_IC_LOAD_NRANK );

#  ifdef LOAD_BALANCE
   Buf_GetBufferData( OPT__UM_IC_LEVEL, amr->FluSg[OPT__UM_IC_LEVEL], NULL_INT, DATA_GENERAL, _TOTAL,
                      Flu_ParaBuf, USELB_YES );
#  endif


// 4. assign data on levels 0 ~ OPT__UM_IC_LEVEL-1 by data restriction
   for (int lv=OPT__UM_IC_LEVEL-1; lv>=0; lv--)
   {
      if ( MPI_Rank == 0 )    Aux_Message( stdout, "   Restricting level %d ... ", lv );

      Flu_Restrict( lv, amr->FluSg[lv+1], amr->FluSg[lv], NULL_INT, NULL_INT, _TOTAL );

#     ifdef LOAD_BALANCE
      LB_GetBufferData( lv, amr->FluSg[lv], NULL_INT, DATA_RESTRICT, _TOTAL, NULL_INT );

      Buf_GetBufferData( lv, amr->FluSg[lv], NULL_INT, DATA_GENERAL, _TOTAL, Flu_ParaBuf, USELB_YES );
#     endif

      if ( MPI_Rank == 0 )    Aux_Message( stdout, "done\n" );
   }



// 5. optimize load-balancing to take into account particle weighting
#  if ( defined PARTICLE  &&  defined LOAD_BALANCE )
   if ( amr->LB->Par_Weight > 0.0 )
      LB_Init_LoadBalance( Redistribute_Yes, amr->LB->Par_Weight, ResetLB_Yes, AllLv );
#  endif



// 6. derefine the uniform-mesh data from levels OPT__UM_IC_LEVEL to 1
#  if ( defined PARTICLE  &&  defined LOAD_BALANCE )
   const double Par_Weight = amr->LB->Par_Weight;
#  else
   const double Par_Weight = 0.0;
#  endif
#  ifdef LOAD_BALANCE
   const UseLBFunc_t UseLB = USELB_YES;
#  else
   const UseLBFunc_t UseLB = USELB_NO;
#  endif

   if ( OPT__UM_IC_DOWNGRADE )
   for (int lv=OPT__UM_IC_LEVEL-1; lv>=0; lv--)
   {
      if ( MPI_Rank == 0 )    Aux_Message( stdout, "   Downgrading level %d ... ", lv+1 );

      Flag_Real( lv, UseLB );

      Refine( lv, UseLB );

#     ifdef LOAD_BALANCE
//    no need to exchange potential since we haven't calculated it yet
      Buf_GetBufferData( lv,   amr->FluSg[lv  ], NULL_INT, DATA_AFTER_REFINE, _TOTAL, Flu_ParaBuf, USELB_YES );

      Buf_GetBufferData( lv+1, amr->FluSg[lv+1], NULL_INT, DATA_AFTER_REFINE, _TOTAL, Flu_ParaBuf, USELB_YES );

      LB_Init_LoadBalance( Redistribute_Yes, Par_Weight, ResetLB_Yes, lv+1 );
#     endif

      if ( MPI_Rank == 0 )    Aux_Message( stdout, "done\n" );
   } // for (int lv=OPT__UM_IC_LEVEL-1; lv>=0; lv--)



// 7. refine the uniform-mesh data from levels OPT__UM_IC_LEVEL to MAX_LEVEL-1
   if ( OPT__UM_IC_REFINE )
   for (int lv=OPT__UM_IC_LEVEL; lv<MAX_LEVEL; lv++)
   {
      if ( MPI_Rank == 0 )    Aux_Message( stdout, "   Refining level %d ... ", lv );

      Flag_Real( lv, UseLB );

      Refine( lv, UseLB );

#     ifdef LOAD_BALANCE
//    no need to exchange potential since we haven't calculated it yet
      Buf_GetBufferData( lv,   amr->FluSg[lv  ], NULL_INT, DATA_AFTER_REFINE, _TOTAL, Flu_ParaBuf, USELB_YES );

      Buf_GetBufferData( lv+1, amr->FluSg[lv+1], NULL_INT, DATA_AFTER_REFINE, _TOTAL, Flu_ParaBuf, USELB_YES );

      LB_Init_LoadBalance( Redistribute_Yes, Par_Weight, ResetLB_Yes, lv+1 );
#     endif

      if ( MPI_Rank == 0 )    Aux_Message( stdout, "done\n" );
   } // for (int lv=OPT__UM_IC_LEVEL; lv<MAX_LEVEL; lv++)


   if ( MPI_Rank == 0 )    Aux_Message( stdout, "%s ... done\n", __FUNCTION__ );

} // FUNCTION : Init_ByFile



//-------------------------------------------------------------------------------------------------------
// Function    :  Init_ByFile_AssignData
// Description :  Use the input uniform-mesh array stored in the file "UM_Filename" to assign data to all
//                real patches on level "UM_lv"
//
// Note        :  1. The function pointer Init_ByFile_User_Ptr() points to Init_ByFile_Default() by default
//                   but may be overwritten by various test problem initializers
//
// Parameter   :  UM_Filename  : Target file name
//                UM_lv        : Target AMR level
//                UM_NVar      : Number of variables
//                UM_LoadNRank : Number of parallel I/O
//
// Return      :  amr->patch->fluid
//-------------------------------------------------------------------------------------------------------
void Init_ByFile_AssignData( const char UM_Filename[], const int UM_lv, const int UM_NVar, const int UM_LoadNRank )
{

   if ( MPI_Rank == 0 )    Aux_Message( stdout, "   Loading data from the input file ...\n" );


// check
   if ( Init_ByFile_User_Ptr == NULL )  Aux_Error( ERROR_INFO, "Init_ByFile_User_Ptr == NULL !!\n" );


   const int    UM_Size3D[3] = { NX0_TOT[0]*(1<<UM_lv),
                                 NX0_TOT[1]*(1<<UM_lv),
                                 NX0_TOT[2]*(1<<UM_lv) };
   const int    scale        = amr->scale[UM_lv];
   const double dh           = amr->dh[UM_lv];

   long   Offset3D_File0[3], Offset_File0, Offset_File, Offset_PG;
   real   fluid[NCOMP_TOTAL];
   double x, y, z;

   real *PG_Data = new real [ CUBE(PS2)*UM_NVar ];


// load data with UM_LoadNRank ranks at a time
   for (int TRank0=0; TRank0<MPI_NRank; TRank0+=UM_LoadNRank)
   {
      if ( MPI_Rank >= TRank0  &&  MPI_Rank < TRank0+UM_LoadNRank )
      {
         if ( MPI_Rank == TRank0 )  Aux_Message( stdout, "      Loading ranks %4d -- %4d ... ",
                                                 TRank0, MIN(TRank0+UM_LoadNRank-1, MPI_NRank-1) );

         FILE *File = fopen( UM_Filename, "rb" );

//       load one patch group at a time
         for (int PID0=0; PID0<amr->NPatchComma[UM_lv][1]; PID0+=8)
         {
//          calculate the file offset of the target patch group
            for (int d=0; d<3; d++)    Offset3D_File0[d] = amr->patch[0][UM_lv][PID0]->corner[d] / scale;

            Offset_File0  = IDX321( Offset3D_File0[0], Offset3D_File0[1], Offset3D_File0[2], UM_Size3D[0], UM_Size3D[1] );
            Offset_File0 *= (long)UM_NVar*sizeof(real);


//          load data from the disk (one row at a time)
            Offset_PG = 0;

            for (int k=0; k<PS2; k++)
            for (int j=0; j<PS2; j++)
            {
               Offset_File = Offset_File0 + (long)UM_NVar*sizeof(real)*( ((long)k*UM_Size3D[1] + j)*UM_Size3D[0] );

               fseek( File, Offset_File, SEEK_SET );
               fread( PG_Data+Offset_PG, sizeof(real), UM_NVar*PS2, File );

//             verify that the file size is not exceeded
               if ( feof(File) )   Aux_Error( ERROR_INFO, "reaching the end of the file \"%s\" !!\n", UM_Filename );

               Offset_PG += UM_NVar*PS2;
            }


//          copy data to each patch
            for (int LocalID=0; LocalID<8; LocalID++)
            {
               const int PID    = PID0 + LocalID;
               const int Disp_i = TABLE_02( LocalID, 'x', 0, PS1 );
               const int Disp_j = TABLE_02( LocalID, 'y', 0, PS1 );
               const int Disp_k = TABLE_02( LocalID, 'z', 0, PS1 );

               for (int k=0; k<PS1; k++)  {  z = amr->patch[0][UM_lv][PID]->EdgeL[2] + (k+0.5)*dh;
               for (int j=0; j<PS1; j++)  {  y = amr->patch[0][UM_lv][PID]->EdgeL[1] + (j+0.5)*dh;
               for (int i=0; i<PS1; i++)  {  x = amr->patch[0][UM_lv][PID]->EdgeL[0] + (i+0.5)*dh;

                  Offset_PG = (long)UM_NVar*IDX321( i+Disp_i, j+Disp_j, k+Disp_k, PS2, PS2 );

                  Init_ByFile_User_Ptr( fluid, PG_Data+Offset_PG, UM_NVar, x, y, z, Time[UM_lv], UM_lv, NULL );

                  for (int v=0; v<NCOMP_TOTAL; v++)
                     amr->patch[ amr->FluSg[UM_lv] ][UM_lv][PID]->fluid[v][k][j][i] = fluid[v];
               }}}
            } // for (int LocalID=0; LocalID<8; LocalID++)
         } // for (int PID0=0; PID0<amr->NPatchComma[UM_lv][1]; PID0+=8)

         fclose( File );

         if ( MPI_Rank == TRank0 )  Aux_Message( stdout, "done\n" );
      } // if ( MPI_Rank >= TRank0  &&  MPI_Rank < TRank0+UM_LoadNRank )

      MPI_Barrier( MPI_COMM_WORLD );
   } // for (int TRank0=0; TRank0<MPI_NRank; TRank0+=UM_LoadNRank)

   delete [] PG_Data;


   if ( MPI_Rank == 0 )    Aux_Message( stdout, "   Loading data from the input file ... done\n" );

} // FUNCTION : Init_ByFile_AssignData



//-------------------------------------------------------------------------------------------------------
// Function    :  Init_ByFile_Default
// Description :  Function to actually set the fluid field from the input uniform-mesh array
//
// Note        :  1. Invoked by Init_ByFile_AssignData() using the function pointer Init_ByFile_User_Ptr()
//                   --> The function pointer may be reset by various test problem initializers, in which case
//                       this funtion will become useless
//                2. Does not floor and normalize passive scalars
//                3. Does not calculate the dual-energy variable
//                   --> When adopting DUAL_ENERGY, the input uniform-mesh array must include the dual-energy
//                       variable
//                4. Assuming nvar_in (i.e., OPT__UM_IC_NVAR) == NCOMP_TOTAL
//
// Parameter   :  fluid_out : Fluid field to be set
//                fluid_in  : Fluid field loaded from the uniform-mesh array (UM_IC)
//                nvar_in   : Number of variables in fluid_in
//                x/y/z     : Target physical coordinates
//                Time      : Target physical time
//                lv        : Target AMR level
//                AuxArray  : Auxiliary array
//
// Return      :  fluid_out
//-------------------------------------------------------------------------------------------------------
void Init_ByFile_Default( real fluid_out[], const real fluid_in[], const int nvar_in,
                          const double x, const double y, const double z, const double Time,
                          const int lv, double AuxArray[] )
{

#  ifdef GAMER_DEBUG
   if ( nvar_in != NCOMP_TOTAL )
      Aux_Error( ERROR_INFO, "nvar_in (%d) != NCOMP_TOTAL (%d) !!\n", nvar_in, NCOMP_TOTAL );
#  endif

   for (int v=0; v<nvar_in; v++)    fluid_out[v] = fluid_in[v];

} // Init_ByFile_Default
