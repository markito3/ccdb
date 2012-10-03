import sys
import posixpath
import logging

from ccdb.Model import Directory, TypeTable
from ccdb.AlchemyProvider import AlchemyProvider
from ccdb.cmd import ConsoleUtilBase
from ccdb.cmd import Theme
from ccdb.cmd import is_verbose, is_debug_verbose

log = logging.getLogger("ccdb.cmd.utils.ls")

#ccdbcmd module interface
def create_util_instance():
    log.debug("      registering ListUtil")
    return List()

#*********************************************************************
#   Class List - List objects in a given directory                   *
#                                                                    *
#*********************************************************************
class List(ConsoleUtilBase):
    """ List objects in a given directory """
    
    # ccdb utility class descr part 
    #------------------------------
    command = "ls"
    name = "List"
    short_descr = "List objects in a given directory"
    uses_db = True

    #variables for each process

    raw_entry = "/"       #object path with possible pattern, like /mole/*
    parent_path = "/"    #parent path
    parent_dir = None    # @type parent_dir DDirectory
    pattern = ""         #pattern on the end of parent path like file?*

    def process(self, args):
        log.debug("ListUtil is gained a control over the process.")
        log.debug("Arguments: " + " ".join(args))
        
        assert self.context is not None
        provider = self.context.provider
        assert isinstance(provider, AlchemyProvider)

        #PARSE ARGUMENTS
        #-------------------------------
        if "--dump-tree" in args:
            self.parent_dir = provider.get_root_directory()
            self.print_directory_tree(self.parent_dir, False, 0)
            return

        if "--dump" in args:
            self.parent_dir = provider.get_root_directory()
            self.print_directory_tree(self.parent_dir, True, 0)
            return

        if len(args) > 0:
            self.raw_entry = args[0]
        else:
            self.raw_entry = self.context.current_path

        #prepare path
        self.raw_entry = self.prepare_path(self.raw_entry)
        
        #SEARCH LOGIC

        #brute assumption that user has entered a simple dir path
        self.parent_dir = provider.get_directory(self.raw_entry)
        self.parent_path = self.raw_entry
        self.pattern = ""

        if not self.parent_dir:
            #we have not find the directory by brute rawentry.
            #but maybe it is just /path/plus*some*pattern
            (head, tale) = posixpath.split(self.raw_entry)
            self.parent_path = head
            self.pattern = tale
            
            if(is_debug_verbose()):
                print "new path: "
                print "   ", self.parent_path
                if len(self.pattern): print "pattern: ", self.pattern

            #try to find such dir once more
            self.parent_dir = provider.get_directory(self.parent_path)

        #found a directory
        if self.parent_dir:
            assert isinstance(self.parent_dir, Directory)

            if(is_debug_verbose()):
                print "full path: \n   ", self.parent_dir.path
                if len(self.pattern): print "pattern: ", self.pattern
            
            #part 1 directories for this path
            sub_dirs = []
            if self.pattern == "":
                sub_dirs = self.parent_dir.sub_dirs
            else:
                sub_dirs = self.context.provider.search_directories(self.pattern, self.parent_path)
           
            for subdir in sub_dirs:
                print Theme.Directories + subdir.name + "    "
            
            #part 2 is tables for this path
            tables = []
            if self.pattern == "":
                tables = self.context.provider.get_type_tables(self.parent_dir)
            else:
                tables = self.context.provider.search_type_tables(self.pattern, self.parent_path)
                        
            for table in tables:
                print table.name
            print
        else:
            print "Can't find the directory"


    def get_name_pathes(self, path):
        self.raw_entry = path
        assert self.context is not None
        provider = self.context.provider
        assert isinstance(provider, AlchemyProvider)

        #prepare path
        self.raw_entry = self.prepare_path(self.raw_entry)
        
        table_list = []
        dir_list = []
        
        
        
        #SEARCH LOGIC
        #---------------------------

        #brute assumption that user has entered a simple dir path
        self.parent_dir = provider.get_directory(self.raw_entry)
        self.parent_path = self.raw_entry
        self.pattern = ""

        if not self.parent_dir:
            #we have not find the directory by brute rawentry.
            #but maybe it is just /path/plus*some*pattern
            (head, tale) = posixpath.split(self.raw_entry)
            self.parent_path = head
            self.pattern = tale
            
            if(is_debug_verbose()):
                print "new path: "
                print "   ", self.parent_path
                if len(self.pattern): print "pattern: ", self.pattern

            #try to find such dir once more
            self.parent_dir = provider.get_directory(self.parent_path)

        #found a directory
        if self.parent_dir:
            assert isinstance(self.parent_dir, Directory)
            assert isinstance(self.context.provider, AlchemyProvider)

            if(is_debug_verbose()):
                print "full path: \n   ", self.parent_dir.path
                if len(self.pattern): print "pattern: ", self.pattern
            
            #part 1 directories for this path
            sub_dirs = []
            if self.pattern == "":
                sub_dirs = self.parent_dir.sub_dirs
            else:
                sub_dirs = self.context.provider.search_directories(self.pattern, self.parent_path)
           
            #fill list of directory names
            dir_list = [subdir.name for subdir in sub_dirs]
            
            #part 2 is tables for this path
            tables = []
            if self.pattern == "":
                tables = self.context.provider.get_type_tables(self.parent_dir)
            else:
                tables = self.context.provider.search_type_tables(self.pattern, self.parent_path)
                        
            #fill list of tables
            table_list=[table.name for table in tables]
            
        else:
            return None
        return (dir_list, table_list)


    def prepare_path(self, path):
        """
        prepares path:
            makes path absolute if it is relative
            removes path artifacts like / in the end, /../ in the middle etc.

        :param path: raw uncoocked path
        :type path: str
        :return: Well done path
        :rtype: str
        """
        #correct ending /

        if path.endswith("/"):
            path = path[:-1]

        #local or absolute path?
        if not path.startswith("/"):
            path = posixpath.join(self.context.current_path , path)

        #normalize
        path = posixpath.normpath(path)

        return path

    def print_directory_tree(self, directory, printFullPath, level):
        """prints a full tree of directories
            This is recursive function"""

        #print this directory
        if not printFullPath:
            print "".join(["   " for i in range(0, level)]) + directory.name
        else:
            print directory.path

        #print subdirectories recursively
        sub_dirs = directory.sub_dirs
        if len(sub_dirs)>0:
            for subDir in sub_dirs:
                self.print_directory_tree(subDir, printFullPath, level+1)

    def print_help(self):
        "Prints help of the command"
        print "Displays directories and tables for current directory"

