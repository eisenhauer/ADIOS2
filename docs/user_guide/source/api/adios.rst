*****
ADIOS
*****

The `adios2::ADIOS` component is the initial point between an application and the ADIOS2 library.

.. code-block:: c++

   /** ADIOS class factory of IO class objects, DebugON is default */
   adios2::ADIOS adios("config.xml", MPI_COMM_WORLD, adios2::DebugON);

The `adios2::ADIOS` component is created passing a **MPI communicator**, which determines the scope of the ADIOS library in an MPI applications, and a **debug mode flag**. 

.. caution::

   Running with the default debug mode (turned on) is highly recommended as it performs additional exception checks that the user can fix in case of an error or ill-formed object (std::invalid_argument). Turning the debug mode option should be used with caution when the user inputs to the resulting workflow is stable.
   
.. note::

   Unexpected system failures and runtime errors are always checked by throwing std::runtime_error. Keep in mind that Segmentation Faults are NOT runtime exceptions. We try to keep user interactions as friendly as possible, please report if any bugs on github: https://github.com/ornladios/ADIOS2/issues  
   

`adios2::ADIOS` objects can be created in MPI and non-MPI (serial) mode. Optionally, a runtime configuration file can be passed to the constructor indicating the full file path, name and extension. Thus resulting in:

**Constructors for MPI applications**

.. code-block:: c++

    /** Constructors */
    adios2::ADIOS (const std::string configFile, 
                   MPI_COMM mpiComm = MPI_COMM_SELF, 
                   const bool debugMode = adios2::DebugON );
                   
    adios2::ADIOS (MPI_COMM mpiComm = MPI_COMM_SELF, 
                   const bool debugMode = adios2::DebugON );

    /** Examples */
    adios2::ADIOS adios(MPI_COMM_WORLD);
    adios2::ADIOS adios("config.xml", MPI_COMM_WORLD, adios2::DebugOFF);

**Constructors for non-MPI (serial) applications**

.. code-block:: c++

    /** Constructors */
    adios2::ADIOS (const std::string configFile, 
                   const bool debugMode = adios2::DebugON );
                   
    adios2::ADIOS (const bool debugMode = adios2::DebugON );

    /** Examples */
    adios2::ADIOS adios("config.xml", adios2::DebugON);
    adios2::ADIOS adios; // Do not use () for empty constructor.

.. tip::
   
   adios2::DebugON and adios::DebugOFF are aliases to true and false, respectively. Use them for code clarity. 


**Factory of IO components**: Multiple IO components (IO tasks) can be created from within the scope of an ADIOS object by calling the **DeclareIO** function:

.. code-block:: c++

    /** Signature */
    adios2::IO ADIOS::DeclareIO(const std::string ioName);

    /** Examples */
    adios2::IO bpWriter = adios.DeclareIO("BPWriter");
    adios2::IO bpReader = adios.DeclareIO("BPReader");
    

This function returns a reference to an existing IO class object that lives inside the ADIOS object that created it. The ioName identifier input must be unique for each IO. Trying to declare an IO object with the same name twice will throw an exception if the debugMode is on in the ADIOS object constructor.

As shown in the diagram below, each resulting IO object is self-managed and independent, thus providing an adaptable way to perform different kinds of I/O operations. Users must be careful not to create conflicts between system level unique I/O identifiers: file names, IP address and port, MPI Send/Receive message rank and tag, etc.

.. blockdiag::
   
   diagram {
      default_fontsize = 18;
      default_shape = roundedbox;
      default_linecolor = blue;
      span_width = 150;
          
      ADIOS -> IO_1, B, IO_N [label = "DeclareIO",fontsize = 13]; 
      B [shape = "dots"]; 
      ADIOS -> B [style = "none"];
   }
