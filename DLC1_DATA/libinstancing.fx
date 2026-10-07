
// Instancing dependant macros

#ifdef COMPILE_INSTANCING_VERSION

	#define INSTANCES( name, numInstances )	name[ numInstances ]
	
	#define DECLARE_INSTANCE_SUPPORT		int2 instanceNum : COLOR1;
	
	#define INSTANCE_GET( name )			name[ In.instanceNum.x ]
	
#else

	#define INSTANCES( name, numInstances )	name
	
	#define DECLARE_INSTANCE_SUPPORT		
	
	#define INSTANCE_GET( name )			name
	
#endif


	
	
		

