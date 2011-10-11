#include <stdlib.h>
#include <iostream>
#include <sstream>
#include <time.h>
#include <string.h>
#include "DCCDBGlobals.h"
#include "DStringUtils.h"
#include "Providers/DMySQLDataProvider.h"
#include "DLog.h"
#include "Model/DConstantsTypeTable.h"
#include "Model/DRunRange.h"
#include <limits.h>

using namespace ccdb;

#pragma region constructors

ccdb::DMySQLDataProvider::DMySQLDataProvider(void)
{
	mIsConnected = false;
	mMySQLHnd=NULL;
	mResult=NULL;
	mRootDir = new DDirectory(this, this);
	mDirsAreLoaded = false;
	mLastFullQuerry="";
	mLastShortQuerry=""; 
}


ccdb::DMySQLDataProvider::~DMySQLDataProvider(void)
{
	if(IsConnected())
	{
		Disconnect();
	}
}
#pragma endregion constructors

#pragma region Connection

bool ccdb::DMySQLDataProvider::Connect( std::string connectionString )
{
	ClearErrors(); //Clear error in function that can produce new ones

	//Setting default connection	
	DMySQLConnectionInfo connection;
	connection.UserName.assign(CCDB_DEFAULT_MYSQL_USERNAME);
	connection.Password.assign(CCDB_DEFAULT_MYSQL_PASSWORD);
	connection.HostName.assign(CCDB_DEFAULT_MYSQL_URL);
	connection.Database.assign(CCDB_DEFAULT_MYSQL_DATABASE);
	connection.Port	= CCDB_DEFAULT_MYSQL_PORT;

	//try to parse connection string
	if(!ParseConnectionString(connectionString, connection))
	{
		Error(CCDB_ERROR_PARSE_CONNECTION_STRING, "DMySQLDataProvider::Connect()", "Error parse mysql string");
		return false;
	}
						
	//try to connect
	return Connect(connection);
}

bool ccdb::DMySQLDataProvider::Connect(DMySQLConnectionInfo connection)
{
	ClearErrors(); //Clear error in function that can produce new ones

	//check if we are connected
	if(IsConnected())
	{
		Error(CCDB_ERROR_CONNECTION_ALREADY_OPENED, "DMySQLDataProvider::Connect(DMySQLConnectionInfo)", "Connection already opened");
		return false;
	}


	//verbose...
	DLog::Verbose("ccdb::DMySQLDataProvider::Connect", DStringUtils::Format("Connecting to database:\n UserName: %s \n Password: %i symbols \n HostName: %s Database: %s Port: %i",
					connection.UserName.c_str(), connection.Password.length(), connection.HostName.c_str(), connection.Database.c_str(), connection.Port) );
					
	//init connection variable
	if(mMySQLHnd == NULL)mMySQLHnd = mysql_init(NULL);
	if(mMySQLHnd == NULL)
	{
		Error(CCDB_ERROR_CONNECTION_INITIALIZATION, "DMySQLDataProvider::Connect(DMySQLConnectionInfo)", "mysql_init() returned NULL, probably memory allocation problem");
		return false;
	}
	
	//Try to connect to server
	if(!mysql_real_connect (
		mMySQLHnd,						//pointer to connection handler
		connection.HostName.c_str(),	//host to connect to
		connection.UserName.c_str(),	//user name
		connection.Password.c_str(),	//password
		connection.Database.c_str(),	//database to use
		connection.Port, 				//port
		NULL,							//socket (use default)
		0))								//flags (none)
	{
		string errStr = ComposeMySQLError("mysql_real_connect()");
		Error(CCDB_ERROR_CONNECTION_EXTERNAL_ERROR,"bool DMySQLDataProvider::Connect(DMySQLConnectionInfo)",errStr.c_str());

		mMySQLHnd=NULL;		//some compilers dont set NULL after delete
		return false;
	}
	mIsConnected = true;
	return true;
}

bool ccdb::DMySQLDataProvider::ParseConnectionString(std::string conStr, DMySQLConnectionInfo &connection)
{
	//first check for uri type
	int typePos = conStr.find("mysql://");
	if(typePos==string::npos)
	{
		return false;
	}

	//ok we dont need mysql:// in the future. Moreover it will mess our separation logic
	conStr.erase(0,8);

	//then if there is '@' that separates login/password part of uri
	int atPos = conStr.find('@');
	if(atPos!=string::npos)
	{
		string logPassStr;

		//ok! we have it! 
		//take it... but with caution
		if(atPos == conStr.length()-1)
		{
			//it is like 'login:pwd@' string
			conStr=string("");
			logPassStr=conStr.substr(0,atPos);
		}
		else if(atPos==0)
		{
			//it is like '@localhost' string
			conStr=conStr.substr(1);
			logPassStr = string("");
		}
		else
		{
			//a regular case
			logPassStr = conStr.substr(0,atPos);
			conStr=conStr.substr(atPos+1);			
		}

		//is it only login or login&&password?
		int colonPos = logPassStr.find(':');
		if(colonPos!=string::npos)
		{
			connection.UserName = logPassStr.substr(0,colonPos);
			connection.Password = logPassStr.substr(colonPos+1);
		}
		else
		{
			connection.UserName = logPassStr;
		}
	}

	//ok, now we have only "address:port database" part of the string

	//1) deal with database;
	int whitePos=conStr.find(' ');
	if(whitePos!=string::npos)
	{
		connection.Database = conStr.substr(whitePos+1);
		conStr.erase(whitePos);
	}

	//2) deal with port
	int colonPos = conStr.find(':');
	if(colonPos!=string::npos)
	{
		string portStr=conStr.substr(colonPos+1);
		conStr.erase(colonPos);

		connection.Port =atoi(portStr.c_str());
	}

	//3) everything that is last whould be address
	connection.HostName = conStr;

	return true;
}


bool ccdb::DMySQLDataProvider::IsConnected()
{
	return mIsConnected;
}


void ccdb::DMySQLDataProvider::Disconnect()
{
	if(IsConnected())
	{
		FreeMySQLResult();	//it would free the result or do nothing
		
		mysql_close(mMySQLHnd);
		mMySQLHnd = NULL;
		mIsConnected = false;
	}
}

bool ccdb::DMySQLDataProvider::CheckConnection( const string& errorSource/*=""*/ )
{
	ClearErrors(); //Clear error in function that can produce new ones

	//check connection
	if(!IsConnected())
	{
		Error(CCDB_ERROR_NOT_CONNECTED,errorSource.c_str(), "Provider is not connected to MySQL.");
		return false;
	}
	return true;

}
#pragma endregion Connection

#pragma region Directories

bool ccdb::DMySQLDataProvider::MakeDirectory( const string& newDirName, const string& parentDirFullPath, const string& comment /*= ""*/ )
{
	ClearErrors(); //Clear error in function that can produce new ones

	//validate symbols in directory name
	string name(newDirName);

	if(!ValidateName(name))
	{
		Error(CCDB_ERROR_INVALID_OBJECT_NAME,"ccdb::DMySQLDataProvider::MakeDirectory", "Object name format is invalid.");
		return false;
	}

	//get parent directory
	DDirectory * parentDir = GetDirectory(parentDirFullPath);
	if(parentDir == NULL)
	{
		Error(CCDB_ERROR_NO_PARENT_DIRECTORY,"DMySQLDataProvider::MakeDirectory", "Provider is not connected to MySQL.");
		return false;
	}

	//maybe such directory already exists?
	string fullPath = DStringUtils::CombinePath(parentDir->GetFullPath() ,name);
	DDirectory * dir = GetDirectory(fullPath.c_str());
	if(dir)
	{
		Error(CCDB_ERROR_DIRECTORY_EXISTS,"DMySQLDataProvider::MakeDirectory", "Such directory already exists");
		return false;
	}
	
	//ok... maybe table with such name exist? 
	DConstantsTypeTable *tmpTable = GetConstantsTypeTable(fullPath);
	if(tmpTable)
	{
		delete tmpTable; 
		//error? Warning?
		Error(CCDB_ERROR_TABLE_EXISTS,"DMySQLDataProvider::MakeDirectory", "Table with this name already exists");
		return false;
	}
	//building such query
	string commentInsertion = PrepareCommentForInsert(comment);	//might be NULL or \"<comment>\"
	string query = DStringUtils::Format(						
		"INSERT INTO `directories` (`modified`, `name`, `parentId`, `comment`) VALUES (NULL, '%s', '%i', %s);", 
		name.c_str(), 
		parentDir->GetId(), 
		commentInsertion.c_str());

	//query DB! 
	bool result = QueryInsert(query.c_str()); 

	if(result)
	{
		//here we save parent name because next, when LoadDirectories will be called link to directory will become junk 
		string parentName = parentDir->GetName();

		//Here we might create new DDirectory * and add it to parent directory,
		//It is extremely fast comparing to database requests
		//but! there some values that might be automatically set by MYSQL
		//assuming that adding directory is "handmade" and sole operation
		//we just reload all directories from DB;
		LoadDirectories();

		//And log such function
		AddLogRecord("directories;", 
			DStringUtils::Format("directories_%i;", mLastInsertedId), 
			DStringUtils::Format("Directory %s created in %s", name.c_str(), parentName.c_str()),
			DStringUtils::Encode(DStringUtils::Format("Related comments: %s", commentInsertion.c_str())));
	}
	
	return result;
}


DDirectory* ccdb::DMySQLDataProvider::GetDirectory( const string& path )
{
	//Mayby we need to update our directories?
	UpdateDirectoriesIfNeeded();
	
	//search full path
	map<string, DDirectory*>::iterator it = mDirectoriesByFullPath.find(string(path));

	//found?
	if(it == mDirectoriesByFullPath.end()) return NULL; //not found
	return it->second; //found
}


bool ccdb::DMySQLDataProvider::UpdateDirectory( DDirectory *dir )
{
	ClearErrors(); //Clear error in function that can produce new ones

	//Check id! 
	if(dir->GetId() <= 0 )
	{
		//TODO: report error
		Error(CCDB_ERROR_DIRECTORY_INVALID_ID,"DMySQLDataProvider::UpdateDirectory", "Id <= 0");
		return false;
	}

	string query = DStringUtils::Format("UPDATE `directories` SET `modified` = NULL, `name` = '%s', `parentId` = %i, `comment` = %s, WHERE `id` = '%i'",
		/*name */		DStringUtils::Encode(dir->GetName()).c_str(),
		/*partntId */	dir->GetParentId(),
		/*comment */	PrepareCommentForInsert(dir->GetComment().c_str()).c_str(),
		/*id */			dir->GetId());
	return QueryUpdate(query);
}

bool ccdb::DMySQLDataProvider::DeleteDirectory( const string& fullPath )
{
	ClearErrors(); //Clear error in function that can produce new ones

	DDirectory *dir = GetDirectory(fullPath);
	if(!dir)
	{
		Error(CCDB_ERROR_DIRECTORY_NOT_FOUND,"DMySQLDataProvider::DeleteDirectory", "Directory not found with this path");
		return false;
	}
	return DeleteDirectory(dir);
}

bool ccdb::DMySQLDataProvider::DeleteDirectory( DDirectory *dir )
{
	ClearErrors(); //Clear error in function that can produce new ones

	//check not NULL
	if(dir==NULL)
	{
		Error(CCDB_ERROR_DIRECTORY_NOT_FOUND,"DMySQLDataProvider::DeleteDirectory", "Directory not found with this path");
		return false;
	}

	//check id...
	if(dir->GetId()<=0)
	{
		//TODO: ERROR report
		Error(CCDB_ERROR_DIRECTORY_INVALID_ID,"DMySQLDataProvider::DeleteDirectory", "Directory invalid id");

		return false;
	}
	
	if(dir->GetSubdirectories().size()>0)
	{
		//TODO error, have children
		Error(CCDB_ERROR_DELETE_NONEMPTY,"DMySQLDataProvider::DeleteDirectory", "Directory contains subdirectory");
		return false;
	}
	
	string assCountQuery = DStringUtils::Format("SELECT `id` FROM `typeTables` WHERE `directoryId`='%i' LIMIT 1",dir->GetId());
	if(!QuerySelect(assCountQuery)) 
	{
		return false;
	}
	if(mReturnedRowsNum > 0)
	{
		//TODO warning
		Error(CCDB_ERROR_DELETE_NONEMPTY,"DMySQLDataProvider::DeleteDirectory", "Directory contains type tables");
		return false;
	}
	string affectedIds;
	
	string query = DStringUtils::Format("DELETE FROM `directories` WHERE id = '%i';", dir->GetId());
	
	bool result =  QueryDelete(query.c_str());
	if(result)
	{
		//just log this wicked action
		AddLogRecord("directories;",
		DStringUtils::Format("directories_%l;", dir->GetId()),
		DStringUtils::Format("Delete directory %s", dir->GetName().c_str()),
		DStringUtils::Format("Delete directory %s,\n comments: %s", dir->GetName().c_str(), dir->GetComment().c_str()));
	}
	LoadDirectories();

	return result;
}

#pragma endregion Directories



bool ccdb::DMySQLDataProvider::IsStoredObjectsOwner()
{
	return mIsStoredObjectOwner;
}

void ccdb::DMySQLDataProvider::IsStoredObjectsOwner(bool flag)
{
	mIsStoredObjectOwner = flag;
}

DConstantsTypeTable * ccdb::DMySQLDataProvider::GetConstantsTypeTable( const string& name, DDirectory *parentDir,bool loadColumns/*=false*/ )
{
	ClearErrors(); //Clear error in function that can produce new ones

	//check the directory is ok
	if(parentDir == NULL || parentDir->GetId()<=0)
	{
		//error
		Error(CCDB_ERROR_NO_PARENT_DIRECTORY,"DMySQLDataProvider::GetConstantsTypeTable", "Parent directory is null or have invalid ID");
		return NULL;
	}

	//check connection
	if(!IsConnected())
	{
		//error !not connect
		Error(CCDB_ERROR_NOT_CONNECTED,"DMySQLDataProvider::GetConstantsTypeTable", "Provider is not connected to MySQL.");
		return NULL;
	}
	
	string query = DStringUtils::Format("SELECT `id`, UNIX_TIMESTAMP(`created`) as `created`, UNIX_TIMESTAMP(`modified`) as `modified`, `name`, `directoryId`, `nRows`, `nColumns`, `comments` FROM `typeTables` WHERE `name` = '%s' AND `directoryId` = '%i';",
		 /*`name`*/ name.c_str(),
		 /*`directoryId`*/ parentDir->GetId());

	if(!QuerySelect(query))
	{
		//TODO: report error
		return NULL;
	}

	
	//Ok! We querryed our directories! lets catch them! 
	if(!FetchRow())
	{
		//TODO error not selected
		return NULL;
	}

	//ok lets read the data...
	DConstantsTypeTable *result = new DConstantsTypeTable(this, this);
	result->SetId(ReadULong(0));
	result->SetCreatedTime(ReadUnixTime(1));
	result->SetModifiedTime(ReadUnixTime(2));
	result->SetName(ReadString(3));
	result->SetDirectoryId(ReadULong(4));
	result->SetNRows(ReadInt(5));
	result->SetNColumnsFromDB(ReadInt(6));
	result->SetComment(ReadString(7));
	
	SetObjectLoaded(result); //set object flags that it was just loaded from DB
	
	result->SetDirectory(parentDir);

	//some validation of loaded record...
	if(result->GetName() == "")
	{
		//TODO error, name should be not null and not empty
		Error(CCDB_ERROR_TYPETABLE_HAS_NO_NAME,"DMySQLDataProvider::GetConstantsTypeTable", "");
		delete result;
		return NULL;
	}
	
	//Ok set a full path for this constant...
	result->SetFullPath(DStringUtils::CombinePath(parentDir->GetFullPath(), result->GetName()));

	
	FreeMySQLResult();
	
	//load columns if needed
	if(loadColumns) LoadColumns(result);
	return result;
}

DConstantsTypeTable * ccdb::DMySQLDataProvider::GetConstantsTypeTable(const string& path, bool loadColumns/*=false*/ )
{
	//get directory path
	string dirPath = DStringUtils::ExtractDirectory(path);
	
	//and directory
	DDirectory *dir = GetDirectory(dirPath.c_str());
	//probably one may wish to check dir to be !=NULL,
	//but such check is in GetConstantsTypeHeader(const char* name, DDirectory *parentDir);
	
	//retrieve name of our constant table 
	string name = DStringUtils::ExtractObjectname(path);
	
	//get it from db etc...
	return GetConstantsTypeTable(name.c_str(), dir, loadColumns);
}

bool ccdb::DMySQLDataProvider::GetConstantsTypeTables( vector<DConstantsTypeTable *>& resultTypeTables, const string&  parentDirPath, bool loadColumns/*=false*/ )
{
	//and directory
	DDirectory *dir = GetDirectory(parentDirPath);
	//probably one may wish to check dir to be !=NULL,
	//but such check is in GetConstantsTypeHeaders( DDirectory *parentDir, vector<DConstantsTypeTable *>& consts );
	return GetConstantsTypeTables(resultTypeTables, dir);
}

bool ccdb::DMySQLDataProvider::GetConstantsTypeTables(  vector<DConstantsTypeTable *>& resultTypeTables, DDirectory *parentDir, bool loadColumns/*=false*/)
{
	ClearErrors(); //Clear error in function that can produce new ones

	//check the directory is ok
	if(parentDir == NULL || parentDir->GetId()<=0)
	{
		//TODO error
		Error(CCDB_ERROR_NO_PARENT_DIRECTORY,"DMySQLDataProvider::GetConstantsTypeTables", "Parent directory is null or has invald ID");
		return NULL;
	}
	
	//Ok, lets cleanup result list
		resultTypeTables.clear(); //we clear the consts. Considering that some one else should handle deletion

	string query = DStringUtils::Format("SELECT `id`, UNIX_TIMESTAMP(`created`) as `created`, UNIX_TIMESTAMP(`modified`) as `modified`, `name`, `directoryId`, `nRows`, `nColumns`, `comments` FROM `typeTables` WHERE `directoryId` = '%i';",
		/*`directoryId`*/ parentDir->GetId());

	if(!QuerySelect(query))
	{
		//no report error
		return NULL;
	}

	//Ok! We querryed our directories! lets catch them! 
	while(FetchRow())
	{
		//ok lets read the data...
		DConstantsTypeTable *result = new DConstantsTypeTable(this, this);
		result->SetId(ReadIndex(0));
		result->SetCreatedTime(ReadUnixTime(1));
		result->SetModifiedTime(ReadUnixTime(2));
		result->SetName(ReadString(3));
		result->SetDirectoryId(ReadULong(4));
		result->SetNRows(ReadInt(5));
		result->SetNColumnsFromDB(ReadInt(6));
		result->SetComment(ReadString(7));
		
		if(loadColumns) LoadColumns(result);
		result->SetDirectory(parentDir);
		
		SetObjectLoaded(result); //set object flags that it was just loaded from DB
		resultTypeTables.push_back(result);
	}

	FreeMySQLResult();

	return true;
}

vector<DConstantsTypeTable *> ccdb::DMySQLDataProvider::GetConstantsTypeTables( const string& parentDirPath, bool loadColumns/*=false*/ )
{
	vector<DConstantsTypeTable *> tables;
	GetConstantsTypeTables(tables, parentDirPath, loadColumns);
	return tables;
}

vector<DConstantsTypeTable *> ccdb::DMySQLDataProvider::GetConstantsTypeTables( DDirectory *parentDir, bool loadColumns/*=false*/ )
{
	
		vector<DConstantsTypeTable *> tables;
		GetConstantsTypeTables(tables, parentDir, loadColumns);
		return tables;
	
}

bool ccdb::DMySQLDataProvider::CreateConstantsTypeTable( DConstantsTypeTable *table )
{
	ClearErrors(); //Clear error in function that can produce new ones

	//validation
	if(table == NULL)
	{
		//TODO error? Warning?
		Error(CCDB_ERROR_NO_TYPETABLE,"DMySQLDataProvider::CreateConstantsTypeTable", "Type table is null or invalid");
		return false;
	}

	if(table->GetDirectory() == NULL)
	{
		//TODO error? Warning?
		Error(CCDB_ERROR_NO_PARENT_DIRECTORY,"DMySQLDataProvider::CreateConstantsTypeTable", "Directory pointer is NULL for this type table object");
		return false;
	}

	if(table->GetColumns().size() <=0)
	{
		//TODO error? Warning?
		Error(CCDB_ERROR_TABLE_NO_COLUMNS,"DMySQLDataProvider::CreateConstantsTypeTable", "No colums for this type table object. Cant create");
		return false;
	}

	if(!ValidateName(table->GetName()))
	{
		//TODO error? Warning?
		Error(CCDB_ERROR_INVALID_OBJECT_NAME,"DMySQLDataProvider::CreateConstantsTypeTable", "Name has invalid format");
		return false;
	}
	if(!table->GetNRows())
	{
		//TODO error? Warning?
		Error(CCDB_ERROR_TABLE_NO_ROWS,"DMySQLDataProvider::CreateConstantsTypeTable", "Nomber of rows is equal to 0");
		return false;
	}

	//ok... maybe table with such name exist? 
	DConstantsTypeTable *tmpTable = GetConstantsTypeTable(table->GetName().c_str(), table->GetDirectory());
	if(tmpTable)
	{
		delete tmpTable; 
		//error? Warning?
		Error(CCDB_ERROR_TABLE_EXISTS,"DMySQLDataProvider::CreateConstantsTypeTable", "Table with this name already exists");
		return false;
	}

	//ok... maybe directory with such name exist? 
	DDirectory *tmpDir = GetDirectory(DStringUtils::CombinePath(table->GetDirectory()->GetFullPath(), table->GetName()) );
	if(tmpDir)
	{	
		//error? Warning?
		Error(CCDB_ERROR_DIRECTORY_EXISTS,"DMySQLDataProvider::CreateConstantsTypeTable", "There is a directory with such name");
		return false;
	}


	//start query, lock tables, make transaction imune;
	if(!QueryCustom("START TRANSACTION;"))
	{
		return false; 
	}
	
	string query = 
	" INSERT INTO `typeTables` "
	"	(`modified`, `name`, `directoryId`, `nRows`, `nColumns`, `comments`) VALUES "
	"	(   NULL   , \"%s\",      '%i'    ,   '%i' ,     '%i'  ,     %s	  ); ";
	
	query = DStringUtils::Format(query.c_str(), 
		table->GetName().c_str(),
		table->GetDirectory()->GetId(),
		table->GetNRows(),
		table->GetNColumns(),
		PrepareCommentForInsert(table->GetComment().c_str()).c_str()  );
	
	//query...
	if(!QueryInsert(query))
	{
		//no report error
		QueryCustom("ROLLBACK;"); //rollback transaction doesnt matter will it happen or not but we should try
		return false;
	}
	
	table->SetId(static_cast<dbkey_t>(mLastInsertedId));
	
	//Now it is time to create columns
	if(!CreateColumns(table))
	{
		QueryCustom("ROLLBACK;"); //rollback transaction. Will it happen or not depends on error - but we should try
		return false;
	}
	
	//commit changes

	if(!QueryCustom("COMMIT;"))
	{
		return false; 
	}

	//add log record
	AddLogRecord("typeTables;",
		DStringUtils::Format("typeTables_%l;", table->GetId()),
		DStringUtils::Format("Created constants type table %s", table->GetName().c_str()),
		DStringUtils::Format("Created constants type table %s,\n comments: %s", table->GetName().c_str(), table->GetComment().c_str()));
	
	return true;
}

DConstantsTypeTable* ccdb::DMySQLDataProvider::CreateConstantsTypeTable( const string& name, const string& parentPath, int rowsNumber, map<string, string> columns, const string& comments /*=""*/ )
{
	return CreateConstantsTypeTable(name, GetDirectory(parentPath), rowsNumber, columns, comments);
}

DConstantsTypeTable* ccdb::DMySQLDataProvider::CreateConstantsTypeTable( const string& name, DDirectory *parentDir, int rowsNumber, map<string, string> columns, const string& comments /*=""*/ )
{
	DConstantsTypeTable *table = new DConstantsTypeTable();
	table->SetName(name);
	table->SetComment(comments);
	table->SetNRows(rowsNumber);

	map<string, string>::const_iterator iter = columns.begin();
	for(; iter != columns.end(); iter++)
	{
		table->AddColumn(iter->first, DConstantsTypeColumn::StringToType(iter->second));
	}
	table->SetDirectory(parentDir);

	if(CreateConstantsTypeTable(table)) return table;
	else return NULL;
}



bool ccdb::DMySQLDataProvider::SearchConstantsTypeTables( vector<DConstantsTypeTable *>& typeTables, const string& pattern, const string& parentPath /*= ""*/, bool loadColumns/*=false*/, int take/*=0*/, int startWith/*=0 */ )
{
	ClearErrors(); //Clear error in function that can produce new ones

	// in MYSQL compared to wildcards % is * and _ is 
	// convert it. 
	string likePattern = WilcardsToLike(pattern);
	
	//do we need to search only in specific directory?
	string parentAddon(""); 		//this is addon to query indicates this
	DDirectory *parentDir = NULL; //will need it anyway later
	if(parentPath!="")
	{	//we should care about parent path!
		if(parentDir = GetDirectory(parentPath.c_str()))
		{
			parentAddon = DStringUtils::Format(" AND `directoryId` = '%i'", parentDir->GetId());
		}
		else
		{
			//request was made for directory that doestn exits
			//TODO place warning or not?
			Error(CCDB_ERROR_DIRECTORY_NOT_FOUND,"DMySQLDataProvider::SearchConstantsTypeTables", "Path to search is not found");
			return false;
		}
	}
	
	//Ok, lets cleanup result list
	if(typeTables.size()>0)
	{
		vector<DConstantsTypeTable *>::iterator iter = typeTables.begin();
		while(iter != typeTables.end())
		{
			DConstantsTypeTable *obj = *iter;
			if(IsOwner(obj)) delete obj;		//delete objects if this provider is owner
			iter++;	
		}
	}
	typeTables.clear(); //we clear the consts. Considering that some one else  should handle deletion

	string limitAddon = PrepareLimitInsertion(take, startWith);

	//combine query
	string query = DStringUtils::Format("SELECT `id`, UNIX_TIMESTAMP(`created`) as `created`, UNIX_TIMESTAMP(`modified`) as `modified`, `name`, `directoryId`, `nRows`, `nColumns`, `comments` FROM `typeTables` WHERE `name` LIKE '%s' %s ORDER BY `name` %s;",
		likePattern.c_str(), parentAddon.c_str(), limitAddon.c_str());

	if(!QuerySelect(query))
	{
		//no report error
		return NULL;
	}


	//Ok! We querryed our directories! lets catch them! 
	while(FetchRow())
	{
		//ok lets read the data...
		DConstantsTypeTable *result = new DConstantsTypeTable(this, this);
		result->SetId(ReadULong(0));
		result->SetCreatedTime(ReadUnixTime(1));
		result->SetModifiedTime(ReadUnixTime(2));
		result->SetName(ReadString(3));
		result->SetDirectoryId(ReadULong(4));
		result->SetNRows(ReadInt(5));
		result->SetNColumnsFromDB(ReadInt(6));
		result->SetComment(ReadString(7));
		
		result->SetDirectory(parentDir);
		if(loadColumns) LoadColumns(result);
		SetObjectLoaded(result); //set object flags that it was just loaded from DB
		
		typeTables.push_back(result);
		
	}

	FreeMySQLResult();
	
	return true;

}

vector<DConstantsTypeTable *> ccdb::DMySQLDataProvider::SearchConstantsTypeTables( const string& pattern, const string& parentPath /*= ""*/, bool loadColumns/*=false*/, int take/*=0*/, int startWith/*=0 */ )
{
	vector<DConstantsTypeTable *> tables;
	SearchConstantsTypeTables(tables, pattern, parentPath,loadColumns, take, startWith);
	return tables;
}



bool ccdb::DMySQLDataProvider::UpdateConstantsTypeTable( DConstantsTypeTable *table )
{
	ClearErrors(); //Clear error in function that can produce new ones

	if(!table || !table->GetId())
	{
		//TODO warning
		Error(CCDB_ERROR_NO_TYPETABLE,"DMySQLDataProvider::UpdateConstantsTypeTable", "Type table is null or have wrong ID");
		return false;
	}
	
	if(!table->GetDirectory())
	{
		//TODO warning
		Error(CCDB_ERROR_NO_PARENT_DIRECTORY,"DMySQLDataProvider::UpdateConstantsTypeTable", "Directory is NULL for the table");
		return false;
	}
	
	if(!ValidateName(table->GetName()))
	{
		//done error
		Error(CCDB_ERROR_INVALID_OBJECT_NAME,"DMySQLDataProvider::UpdateConstantsTypeTable", "Table name is incorect. Only letters, digits and '_' are allowed. ");
		return false;
	}
	
	DConstantsTypeTable *existingTable =GetConstantsTypeTable(table->GetFullPath());
	
	if(existingTable!=NULL && existingTable->GetId()!=table->GetId())
	{
		//error
		Error(CCDB_ERROR_NO_TYPETABLE,"DMySQLDataProvider::UpdateConstantsTypeTable", "Another table whith such name is found");
		return false;
	}
	
	string query = DStringUtils::Format(" UPDATE `typeTables`"
			" SET `modified` = NULL, `name` = \"%s\", `directoryId` = '%i', `comments` = %s "
			" WHERE `id` = '%i' ", 
			table->GetName().c_str(), 
			table->GetDirectory()->GetId(), 
			PrepareCommentForInsert(table->GetComment()).c_str(),
			table->GetId());
	
	bool result = QueryUpdate(query);
	if(result)
	{
		AddLogRecord("typeTables;",
		DStringUtils::Format("typeTables_%l;", table->GetId()),
		DStringUtils::Format("Update constants type table %s", table->GetName().c_str()),
		DStringUtils::Format("Update constants type table %s,\n comments: %s", table->GetName().c_str(), table->GetComment().c_str()));
	}
	else
	{
		//probably the error is printed by QueryUpdate
		return false;
	}
	
	return true;
}

bool ccdb::DMySQLDataProvider::DeleteConstantsTypeTable( DConstantsTypeTable *table )
{
	ClearErrors(); //Clear error in function that can produce new ones

	//validation
	if(table == NULL || table->GetId() <=0)
	{
		//TODO error? Warning?
		Error(CCDB_ERROR_NO_TYPETABLE,"DMySQLDataProvider::DeleteConstantsTypeTable", "Type table is null or have wrong ID");
		return false;
	}
	
	string assCountQuery = DStringUtils::Format(" SELECT `id` FROM `constantSets` WHERE `constantTypeId`='%i' LIMIT 1", table->GetId() );
	if(!QuerySelect(assCountQuery)) 
	{	
		return false;
	}
	if(mReturnedRowsNum > 0)
	{
		return false;
	}
	FreeMySQLResult();
	

	string query = DStringUtils::Format("DELETE FROM `typeTables` WHERE `id` = %i ;", table->GetId());
	if(!QueryDelete(query))
	{
		return false;
	}

	//Delete columns
	query = DStringUtils::Format("DELETE FROM `columns` WHERE `typeId` = %i ;", table->GetId());
	if(!QueryDelete(query))
	{
		return false;
	}
	else
	{
		//just log this wicked action
		AddLogRecord("typeTables;",
		DStringUtils::Format("typeTables_%l;", table->GetId()),
		DStringUtils::Format("Delete constants type table %s", table->GetName().c_str()),
		DStringUtils::Format("Delete constants type table %s,\n comments: %s", table->GetName().c_str(), table->GetComment().c_str()));
	}
	return true;
}

bool ccdb::DMySQLDataProvider::CreateColumn(DConstantsTypeColumn* column)
{

	/** @brief Creates columns for the table
	 *
	 * @param parentDir
	 * @return vector of constants
	 */
	
	string query = 
		"INSERT INTO `columns`												  "
		"	( `modified`, `name`, `typeId`, `columnType`, `order`, `comment`) "
		"	VALUES															  "
		"	(   NULL    , \"%s\",   '%i'  ,    '%s'     ,  '%i'  ,    %s    );";

	query = DStringUtils::Format(query.c_str(), 
		column->GetName().c_str(), 
		column->GetTypeTableId(),  
		column->GetTypeString().c_str(),
		column->GetOrder(),
		PrepareCommentForInsert(column->GetComment()).c_str());
	
	if(!QueryInsert(query))
	{
		return false;
	}
	
	return true;
}
bool ccdb::DMySQLDataProvider::CreateColumns(DConstantsTypeTable* table)
{
/** @brief Loads columns for the table
 *
 * @param parentDir
 * @return vector of constants
 */
	ClearErrors(); //Clear error in function that can produce new ones

	//get and validate parent table ID
	dbkey_t tableId = table->GetId();
	if(tableId <=0)
	{
		//TODO WARNING try to create columns without table key
		Error(CCDB_ERROR_INVALID_ID,"DMySQLDataProvider::CreateColumns", "Type table has wrong DB ID");
		return false; 
	}

	if(table->GetColumns().size() <=0)
	{
		//TODO WARNING try to create columns without columns
		Error(CCDB_ERROR_TABLE_NO_COLUMNS,"DMySQLDataProvider::CreateColumns", "Table have no columns or colums are not loaded");
		return false; 
	}
	
	//TODO begin transaction

	const vector<DConstantsTypeColumn *>& columns = table->GetColumns();
	vector<DConstantsTypeColumn *>::const_iterator iter= columns.begin();
	for(; iter<columns.end(); ++iter)
	{
		DConstantsTypeColumn *column= *iter;
		if(!(CreateColumn(column )))
		{	
			return false;
		}
	}

	//TODO end transaction
	return true;
}

bool ccdb::DMySQLDataProvider::LoadColumns( DConstantsTypeTable* table )
{
	ClearErrors(); //Clear error in function that can produce new ones

	//check the directory is ok
	if(table->GetId()<=0)
	{
		//TODO error
		Error(CCDB_ERROR_INVALID_ID,"DMySQLDataProvider::LoadColumns", "Type table has wrong ID");
		return false;
	}
		
	string query = DStringUtils::Format("SELECT `id`, UNIX_TIMESTAMP(`created`) as `created`, UNIX_TIMESTAMP(`modified`) as `modified`, `name`, `columnType`, `comment` FROM `columns` WHERE `typeId` = '%i' ORDER BY `order`;",
		/*`directoryId`*/ table->GetId());

	if(!QuerySelect(query))
	{
		return false;
	}

	//clear(); //we clear the consts. Considering that some one else should handle deletion

	//Ok! We querried our directories! lets catch them! 
	while(FetchRow())
	{
		//ok lets read the data...
		DConstantsTypeColumn *result = new DConstantsTypeColumn(table, this);
		result->SetId(ReadULong(0));				
		result->SetCreatedTime(ReadUnixTime(1));
		result->SetModifiedTime(ReadUnixTime(2));
		result->SetName(ReadString(3));
		result->SetType(ReadString(4));
		result->SetComment(ReadString(5));
		result->SetDBTypeTableId(table->GetId());

		SetObjectLoaded(result); //set object flags that it was just loaded from DB

		table->AddColumn(result);
	}

	FreeMySQLResult();

	return true;


}

bool ccdb::DMySQLDataProvider::CreateRunRange( DRunRange *run )
{
	ClearErrors(); //Clear error in function that can produce new ones

	//query;
	string query = DStringUtils::Format("INSERT INTO `runRanges` (`modified`, `runMin`, `runMax`, `name`, `comment`)"
		"VALUES (NULL, '%i', '%i', '%s', '%s');",
		run->GetMin(),
		run->GetMax(),
		run->GetName().c_str(),
		run->GetComment().c_str());
	
	//Do query
	if(!QueryInsert(query))
	{
		//NO report error
		return false;
	}

	return true;
}

DRunRange* ccdb::DMySQLDataProvider::GetRunRange( int min, int max, const string& name /*= ""*/ )
{
	//build query
	string query = "SELECT `id`, UNIX_TIMESTAMP(`created`) as `created`, UNIX_TIMESTAMP(`modified`) as `modified`, `name`, `runMin`, `runMax`,  `comment`"
	               " FROM `runRanges` WHERE `runMin`='%i' AND `runMax`='%i' AND `name`=\"%s\"";
	query = DStringUtils::Format(query.c_str(), min, max, name.c_str());
	
	//query this
	if(!QuerySelect(query))
	{
		//NO report error
		return NULL;
	}
	
	//Ok! We querryed our run range! lets catch it! 
	
	if(!FetchRow())
	{
		//nothing was selected
		return NULL;
	}

	//ok lets read the data...
	DRunRange *result = new DRunRange(this, this);
	result->SetId(ReadULong(0));
	result->SetCreatedTime(ReadUnixTime(1));
	result->SetModifiedTime(ReadUnixTime(2));
	result->SetName(ReadString(3));
	result->SetMin(ReadInt(4));
	result->SetMax(ReadInt(5));
	result->SetComment(ReadString(6));
	
	if(mReturnedRowsNum>1)
	{
		//TODO warning not uniq row
		Error(CCDB_ERROR,"DMySQLDataProvider::GetRunRange", "Run range with such min, max and name is not alone in the DB");
	}

	FreeMySQLResult();
	return result;

}

DRunRange* ccdb::DMySQLDataProvider::GetRunRange( const string& name )
{
	ClearErrors(); //Clear error in function that can produce new ones

	//build query
	string query = "SELECT `id`, UNIX_TIMESTAMP(`created`) as `created`, UNIX_TIMESTAMP(`modified`) as `modified`, `name`, `runMin`, `runMax`,  `comment`"
		" FROM `runRanges` WHERE `name`=\"%s\"";
	query = DStringUtils::Format(query.c_str(), name.c_str());

	//query this
	if(!QuerySelect(query))
	{
		//NO report error
		return NULL;
	}

	//Ok! We querried our run range! lets catch it! 
	if(!FetchRow())
	{
		//nothing was selected
		return NULL;
	}

	//ok lets read the data...
	DRunRange *result = new DRunRange(this, this);
	result->SetId(ReadULong(0));
	result->SetCreatedTime(ReadUnixTime(1));
	result->SetModifiedTime(ReadUnixTime(2));
	result->SetName(ReadString(3));
	result->SetMin(ReadInt(4));
	result->SetMax(ReadInt(5));
	result->SetComment(ReadString(6));

	if(mReturnedRowsNum>1)
	{
		//warning not uniq row
		Error(CCDB_ERROR,"DMySQLDataProvider::GetRunRange", "Run range with such name is not alone in the DB");
	}

	FreeMySQLResult();
	return result;
}

DRunRange* ccdb::DMySQLDataProvider::GetOrCreateRunRange( int min, int max, const string& name/*=""*/, const string& comment/*=""*/ )
{
	//if one gets NULL after that function it is probably only because 
	//run range with such name already exists but have different min and max ranges
	// or, surely, because error with MySQL connection or something like this happened
	DRunRange *result = GetRunRange(min,max, name);
	if(!result)
	{
		//Ok, lets try to create it...
		result = new DRunRange(this, this);
		result->SetRange(min, max);
		result->SetName(string(name));
		result->SetComment(string(comment));
		//TODO deside how to handle comment and time;
		//TODO (!) Maybe change to REPLACE instead of insert?
		//Try to create and return null if it was impossible
		if(!CreateRunRange(result)) return NULL;

	}
	return result;
}


bool ccdb::DMySQLDataProvider::GetRunRanges(vector<DRunRange*>& resultRunRanges, DConstantsTypeTable* table, const string& variation/*=""*/, int take/*=0*/, int startWith/*=0*/)
{
	ClearErrors(); //Clear error in function that can produce new ones

	if(!CheckConnection("DMySQLDataProvider::GetRunRanges(vector<DRunRange*>& resultRunRanges, DConstantsTypeTable* table, int take, int startWith)")) return false;
	
	//validate table
	if(!table || !table->GetId())
	{
		//TODO report error
		Error(CCDB_ERROR_NO_TYPETABLE,"DMySQLDataProvider::GetRunRanges", "Type table is null or have invalid id");
		return false;
	}
	//variation handle
	string variationWhere("");
	if(variation != "")
	{
		variationWhere.assign(DStringUtils::Format(" AND `variations`.`name`=\"%s\" ", variation.c_str()));
	}

	//limits handle 
	string limitInsertion = PrepareLimitInsertion(take, startWith);
	
	//Ok, lets cleanup result list
	if(resultRunRanges.size()>0)
	{
		vector<DRunRange *>::iterator iter = resultRunRanges.begin();
		while(iter != resultRunRanges.end())
		{
			DRunRange *obj = *iter;
			if(IsOwner(obj)) delete obj;		//delete objects if this provider is owner
			iter++;
		}
	}
	resultRunRanges.clear(); //we clear the consts. Considering that some one else should handle deletion


	//ok now we must build our mighty query...
	string query= 
		" SELECT "
		" DISTINCT `runRanges`.`id` as `id`, "
		" UNIX_TIMESTAMP(`runRanges`.`created`) as `created`, "
		" UNIX_TIMESTAMP(`runRanges`.`modified`) as `modified`, "
		" `runRanges`.`name` as `name`, "
		" `runRanges`.`runMin` as `runMin`, "
		" `runRanges`.`runMax` as `runMax`, "
		"`runRanges`.`comment` as `comment` "
		" FROM `typeTables` "
		" INNER JOIN `assignments` ON `assignments`.`runRangeId`= `runRanges`.`id` "
		" INNER JOIN `variations` ON `assignments`.`variationId`= `variations`.`id` "
		" INNER JOIN `constantSets` ON `assignments`.`constantSetId` = `constantSets`.`id` "
		" INNER JOIN `typeTables` ON `constantSets`.`constantTypeId` = `typeTables`.`id` "
		" WHERE `typeTables`.`id` = '%i' "
		" %s "
		" ORDER BY `runRanges`.`runMin` ASC	 %s";
		
	query=DStringUtils::Format(query.c_str(), table->GetId(), variationWhere.c_str(), limitInsertion.c_str());
	
	//query this
	if(!QuerySelect(query))
	{
		//TODO report error
		return false;
	}

	//Ok! We querried our run range! lets catch it! 
	while(FetchRow())
	{
		//ok lets read the data...
		DRunRange *result = new DRunRange(this, this);
		result->SetId(ReadULong(0));
		result->SetCreatedTime(ReadUnixTime(1));
		result->SetModifiedTime(ReadUnixTime(2));
		result->SetName(ReadString(3));
		result->SetMin(ReadInt(4));
		result->SetMax(ReadInt(5));
		result->SetComment(ReadString(7));
		
		SetObjectLoaded(result);
		resultRunRanges.push_back(result);
	}

	FreeMySQLResult();
	return true;
}

bool ccdb::DMySQLDataProvider::DeleteRunRange(DRunRange* run)
{
	ClearErrors(); //Clear errors in function that can produce new ones

	if(!run || run->GetId() <=0)
	{
		//TODO error;
		Error(CCDB_ERROR_RUNRANGE_INVALID,"DMySQLDataProvider::DeleteRunRange", "Runrange is null or have invalid ID");
		return false;
	}
	string assCountQuery = DStringUtils::Format("SELECT `id` FROM `assignments` WHERE `runRangeId`='%i' LIMIT 1",run->GetId() );
	if(!QuerySelect(assCountQuery)) 
	{
		//TODO error;
		return false;
	}
	if(mReturnedRowsNum > 0)
	{
		//TODO warning
		Error(CCDB_ERROR_DELETE_NONEMPTY,"DMySQLDataProvider::DeleteRunRange", "There is assigment that linked to this runrange. Impossible to delete it");
		return false;
	}
	FreeMySQLResult();

	string query = DStringUtils::Format("DELETE FROM `runranges` WHERE `runranges`.`id`='%i';", run->GetId());
	bool result = QueryDelete(query);
	if(result)
	{
		AddLogRecord("runranges;",DStringUtils::Format("runranges_%i;", run->GetId()), "Delete run range", DStringUtils::Format("Delete run range: from %i to %i with name %s", run->GetMin(), run->GetMax(), run->GetName().c_str()));
	}
	return result;
}

bool ccdb::DMySQLDataProvider::UpdateRunRange(DRunRange* run)
{
	ClearErrors(); //Clear error in function that can produce new ones

	if(run==NULL || run->GetId()<=0) 
	{
		//TODO error
		Error(CCDB_ERROR_RUNRANGE_INVALID,"DMySQLDataProvider::UpdateRunRange", "Run range is null or has wrong id");
		return false;
	}

	string query=DStringUtils::Format(
		"UPDATE `runranges` SET `modified` = NULL, " 
		" `name` = \"%s\", `runMin` = '%i', `runMax` = '%i', `comment` = %s "
		" WHERE `runranges`.`id` = '%i' ;",
		DStringUtils::Encode(run->GetName()).c_str(), 
		run->GetMin(),
		run->GetMax(),
		DStringUtils::Encode(run->GetComment()).c_str(),
		run->GetId());
	return QueryUpdate(query);

}


bool ccdb::DMySQLDataProvider::GetVariations(vector<DVariation*>& resultVariations, DConstantsTypeTable* table, int run, int take, int startWith)
{
	ClearErrors(); //Clear error in function that can produce new ones

	if(!CheckConnection("DMySQLDataProvider::GetRunRanges(vector<DRunRange*>& resultRunRanges, DConstantsTypeTable* table, int take, int startWith)")) return false;
	
	//validate table
	if(!table || !table->GetId())
	{
		//TODO report error
		Error(CCDB_ERROR_NO_TYPETABLE,"DMySQLDataProvider::GetVariations", "Type table is null or empty");
		return false;
	}
	
	//run range handle
	string runRangeWhere(""); //Where clause for run range
	if(run != 0)
	{		
		runRangeWhere = DStringUtils::Format(" AND `runRanges`.`runMin` <= '%i' AND `runRanges`.`runMax` >= '%i' ", run, run);
	}

	//limits handle 
	string limitInsertion = PrepareLimitInsertion(take, startWith);
	
	//Ok, lets cleanup result list
	if(resultVariations.size()>0)
	{
		vector<DVariation *>::iterator iter = resultVariations.begin();
		while(iter != resultVariations.end())
		{
			DVariation *obj = *iter;
			if(IsOwner(obj)) delete obj;		//delete objects if this provider is owner
			iter++;
		}
	}
	resultVariations.clear(); //we clear the consts. Considering that some one else should handle deletion


	//ok now we must build our mighty query...
	string query= 
		" SELECT "
		" DISTINCT `variations`.`id` as `varId`, "
		" UNIX_TIMESTAMP(`variations`.`created`) as `created`, "
		" UNIX_TIMESTAMP(`variations`.`modified`) as `modified`, "
		" `variations`.`name` as `name`, "
		" `variations`.`description` as `description`, "
		" `variations`.`comment` as `comment` "
		" FROM `runRanges` "
		" INNER JOIN `assignments`  ON `assignments`.`runRangeId`= `runRanges`.`id` "
		" INNER JOIN `variations`   ON `assignments`.`variationId`= `variations`.`id` "
		" INNER JOIN `constantSets` ON `assignments`.`constantSetId` = `constantSets`.`id` "
		" INNER JOIN `typeTables`   ON `constantSets`.`constantTypeId` = `typeTables`.`id` "
		" WHERE `typeTables`.`id` = '%i' "
		" %s "
		" ORDER BY `runRanges`.`runMin` ASC	 %s";
		
	query=DStringUtils::Format(query.c_str(), table->GetId(), runRangeWhere.c_str(), limitInsertion.c_str());
	
	//query this
	if(!QuerySelect(query))
	{
		//TODO report error
		return false;
	}

	//Ok! We querried our run range! lets catch it! 
	while(FetchRow())
	{
		//ok lets read the data...
		DVariation *result = new DVariation(this, this);
		result->SetId(ReadULong(0));
		result->SetCreatedTime(ReadUnixTime(1));
		result->SetModifiedTime(ReadUnixTime(2));
		result->SetName(ReadString(3));
		result->SetDescription(ReadString(4));
		result->SetComment(ReadString(5));
		
		SetObjectLoaded(result);
		resultVariations.push_back(result);
	}

	FreeMySQLResult();
	return true;
}

vector<DVariation *> ccdb::DMySQLDataProvider::GetVariations( DConstantsTypeTable *table, int run/*=0*/, int take/*=0*/, int startWith/*=0 */ )
{
	vector<DVariation *> resultVariations;
	GetVariations(resultVariations, table, run, take, startWith);
	return resultVariations;
}


DVariation* ccdb::DMySQLDataProvider::GetVariation( const string& name )
{
	ClearErrors(); //Clear error in function that can produce new ones

	//TODO: Implement method
	string query = 
		"SELECT											 "
		"`id`,											 "
		"UNIX_TIMESTAMP(`created`) as `created`,		 "
		"UNIX_TIMESTAMP(`modified`) as `modified`,		 "
		"`name`,										 "
		"`description`,									 "
		"`comment`										 "
		"FROM `variations`								 "
		"WHERE `name`= \"%s\";							 ";
	query = DStringUtils::Format(query.c_str(), name.c_str());
	//query this
	if(!QuerySelect(query))
	{
		//TODO report error
		return NULL;
	}

	//Ok! We querried our run range! lets catch it! 
	if(!FetchRow())
	{
		//nothing was selected
		return NULL;
	}

	//ok lets read the data...
	DVariation *result = new DVariation(this, this);
	result->SetId(ReadULong(0));
	result->SetCreatedTime(ReadUnixTime(1));
	result->SetModifiedTime(ReadUnixTime(2));
	result->SetName(ReadString(3));
	result->SetDescription(ReadString(4));
	result->SetComment(ReadString(5));

	if(mReturnedRowsNum>1)
	{
		//TODO warning not uniq row
	}

	FreeMySQLResult();
	return result;
}

bool ccdb::DMySQLDataProvider::CreateVariation( DVariation *variation )
{
	ClearErrors(); //Clear error in function that can produce new ones

	string query=
		"INSERT INTO `variations` "
		"(`modified`,			  "
		"`name`,				  "
		"`description`,			  "
		"`comment`)				  "
		"VALUES	(NULL, \"%s\",	\"\", %s);";
	
	//TODO decide what to do with description of variation
	query = DStringUtils::Format(query.c_str(), variation->GetName().c_str(), variation->GetComment().c_str());
	
	//Do query
	if(!QueryInsert(query))
	{
		//TODO report error
		return false;
	}

	return true;
}


bool ccdb::DMySQLDataProvider::UpdateVariation( DVariation *variation )
{
	ClearErrors(); //Clear error in function that can produce new ones

	//validate
	if(!variation || variation->GetId()<=0)
	{
		//TODO warning
		Error(CCDB_ERROR_VARIATION_INVALID,"DMySQLDataProvider::UpdateVariation", "Variation is NULL or has bad ID so update operations cant be done");
		return false;
	}
	
	string query = "UPDATE `variations` SET `modified` = NULL, `name` = \"%s\", `comment` = %s, WHERE `id` = %i";
	query = DStringUtils::Format(query.c_str(), variation->GetName().c_str(), PrepareCommentForInsert(variation->GetComment().c_str()).c_str(), variation->GetId());

	if(!QueryUpdate(query))
	{
		//TODO report something
		return false;
	}

	return true;
}


bool ccdb::DMySQLDataProvider::DeleteVariation( DVariation *variation )
{
	ClearErrors(); //Clear error in function that can produce new ones

	//validate
	if(!variation || variation->GetId()<=0)
	{
		//TODO warning
		Error(CCDB_ERROR_VARIATION_INVALID,"DMySQLDataProvider::DeleteVariation", "Variation is NULL or has bad ID so delete operations cant be done");
		return false;
	}
	
	string assCountQuery = DStringUtils::Format("SELECT `id` FROM `assignments` WHERE `variationId`='%i' LIMIT 1",variation->GetId() );
	if(!QuerySelect(assCountQuery)) 
	{
		//TODO error;
		return false;
	}
	if(mReturnedRowsNum > 0)
	{
		//TODO warning
		return false;
	}
	string query = DStringUtils::Format("DELETE FROM `variations` WHERE `id` = %'i'", variation->GetId());

	bool result = QueryDelete(query);
	if(result)
	{
		AddLogRecord("variations;",DStringUtils::Format("variations_%i;", variation->GetId()), "Delete run variation", DStringUtils::Format("Delete variation: name %s", variation->GetName().c_str()));
	}
	return result;
}

//----------------------------------------------------------------------------------------
//	A S S I G N M E N T S
//----------------------------------------------------------------------------------------
	
DAssignment* ccdb::DMySQLDataProvider::GetAssignmentShort(int run, const string& path, const string& variation)
{
	ClearErrors(); //Clear error in function that can produce new ones

	if(!CheckConnection("DMySQLDataProvider::GetAssignmentShort( int run, const char* path, const char* variation, int version /*= -1*/ )")) return NULL;

	//get table! 
	DConstantsTypeTable *table = GetConstantsTypeTable(path, true);
	if(!table)
	{
		//TODO report error
		Error(CCDB_ERROR_NO_TYPETABLE,"DMySQLDataProvider::GetAssignmentShort", "Table with this name was not found");
		return NULL;
	}

	//ok now we must build our mighty query...
	string query=
		" SELECT "
		" `assignments`.`id` AS `asId`,	"
		" UNIX_TIMESTAMP(`assignments`.`created`) as `asCreated`,"
		" UNIX_TIMESTAMP(`assignments`.`modified`) as `asModified`,	"
		" `constantSets`.`vault` AS `blob`"
		" FROM `runRanges` "
		" INNER JOIN `assignments` ON `assignments`.`runRangeId`= `runRanges`.`id`"
		" INNER JOIN `variations` ON `assignments`.`variationId`= `variations`.`id`	"
		" INNER JOIN `constantSets` ON `assignments`.`constantSetId` = `constantSets`.`id`"
		" INNER JOIN `typeTables` ON `constantSets`.`constantTypeId` = `typeTables`.`id`"
		" WHERE "
		" `runRanges`.`runMin` <= '%i'"
		" AND `runRanges`.`runMax` >= '%i'"
		" AND `variations`.`name`=\"%s\""
		" AND `constantSets`.`constantTypeId` = %i"
		" ORDER BY `assignments`.`created` DESC"
		" LIMIT 0,1;";




	query=DStringUtils::Format(query.c_str(), run,run, variation.c_str(), table->GetId());
	//query this
	if(!QuerySelect(query))
	{
		//TODO report error

		return NULL;
	}

	//Ok! We querried our run range! lets catch it! 
	if(!FetchRow())
	{
		//nothing was selected
		return NULL;
	}

	//ok lets read the data...
	DAssignment *result = new DAssignment(this, this);
	result->SetId( ReadIndex(0) );
	result->SetCreatedTime( ReadUnixTime(1) );
	result->SetModifiedTime( ReadUnixTime(2) );
	result->SetRawData( ReadString(3) );
	
	//additional fill
	result->SetRequestedRun(run);
	result->SetTypeTable(table);
	if(IsOwner(table)) table->SetOwner(result); //new ownership?
	

	if(mReturnedRowsNum>1)
	{
		//TODO warning not uniq row
		Error(CCDB_ERROR,"DMySQLDataProvider::GetAssignmentShort", "Many variations return instead of one");
	}

	FreeMySQLResult();
	return result;
}


DAssignment* ccdb::DMySQLDataProvider::GetAssignmentShort(int run, const string& path, int version, const string& variation)
{
	ClearErrors(); //Clear error in function that can produce new ones

	if(!CheckConnection("DMySQLDataProvider::GetAssignmentShort( int run, const char* path, const char* variation, int version /*= -1*/ )")) return NULL;
	
	//get table! 
	DConstantsTypeTable *table = GetConstantsTypeTable(path, true);
	if(!table)
	{
		//TODO report error
		Error(CCDB_ERROR_NO_TYPETABLE,"DMySQLDataProvider::GetAssignmentShort", "No type table with this path was found");
		return NULL;
	}

	//ok now we must build our mighty query...
	string query= 
		" SELECT 													"
		" `assignments`.`id` AS `asId`,								"
		" UNIX_TIMESTAMP(`assignments`.`created`) as `asCreated`,	"
		" UNIX_TIMESTAMP(`assignments`.`modified`) as `asModified`,	"
		" `runRanges`.`id`   AS `runRangeId`, 						"
		" `variations`.`id`  AS `varId`,								"
		" `constantSets`.`id` AS `constId`,							"
		" `constantSets`.`vault` AS `blob`,							"
		" FROM `typeTables` 											"
		" INNER JOIN `assignments` ON `assignments`.`runRangeId`= `runRanges`.`id` "
		" INNER JOIN `variations` ON `assignments`.`variationId`= `variations`.`id` "
		" INNER JOIN `constantSets` ON `assignments`.`constantSetId` = `constantSets`.`id` "
		" INNER JOIN `typeTables` ON `constantSets`.`constantTypeId` = `typeTables`.`id` "
		" WHERE "
		" `runRanges`.`runMin` < '%i' "
		" AND `runRanges`.`runMax` > '%i' "
		" AND (UNIX_TIMESTAMP(`assignments`.`creation`) = 0 OR 1 = 1) "
		" AND `variations`.`name`=\"%s\"	"
		" AND `typeTables`.`id` = %i	"
		" ORDER BY `assignments`.`time` DESC	"
		" LIMIT 0,1; ";
		

	query=DStringUtils::Format(query.c_str(), run,run, variation.c_str(), table->GetId());
	
	//query this
	if(!QuerySelect(query))
	{
		//TODO report error
		return NULL;
	}

	//Ok! We querried our run range! lets catch it! 
	if(!FetchRow())
	{
		//nothing was selected
		return NULL;
	}

	//ok lets read the data...
	DAssignment *result = new DAssignment(this, this);
	result->SetId(ReadULong(0));
	result->SetCreatedTime(ReadUnixTime(1));
	result->SetModifiedTime(ReadUnixTime(2));
	result->SetRunRangeId(ReadInt(3));
	result->SetVariationId(ReadInt(4));
	result->SetDataVaultId(ReadInt(5));
	result->SetRawData(ReadString(6));
	
	//additional fill
	result->SetRequestedRun(run);
	result->SetTypeTable(table);
	
	if(mReturnedRowsNum>1)
	{
		//TODO warning not uniq row
	}

	FreeMySQLResult();
	return result;

}

bool ccdb::DMySQLDataProvider::CreateAssignment(DAssignment *assignment )
{
	ClearErrors(); //Clear error in function that can produce new ones

	//Check runrange...
	DRunRange * runRange= assignment->GetRunRange();
	if(
			!assignment->GetRunRange()
		||	!assignment->GetRunRange()->GetId()
		||	!assignment->GetVariation()
		||	!assignment->GetVariation()->GetId()
		)
	{
		//TODO event range handling! 
		//TODO do we need a warning or error here?
		return false;
	}
	DConstantsTypeTable *table = assignment->GetTypeTable();
	if(table == NULL || !table->GetId())
	{
		//TODO warning
		Error(CCDB_ERROR_NO_TYPETABLE,"DMySQLDataProvider::CreateAssignment", "Table is NULL or has wrong id");
		return false;

	}

	//start query, lock tables, make transaction imune;
	if(!QueryCustom("START TRANSACTION;"))
	{
		return false; 
	}

	//add constants
	string query = 
		" INSERT INTO `constantSets` (`modified`, `vault`, `constantTypeId`) "
	    "                      VALUES(   NULL   ,  \"%s\",	       %i      );";
	query = DStringUtils::Format(query.c_str(), assignment->GetRawData().c_str(), table->GetId());
	
	//query...
	if(!QueryInsert(query))
	{
		//TODO report error
		QueryCustom("ROLLBACK;"); //rollback transaction doesnt matter will it happen or not but we should try
		return false;
	}
	assignment->SetDataVaultId(static_cast<dbkey_t>(mLastInsertedId));

	query = "INSERT INTO `assignments` "
	"	(				"
	"	`modified`,		"
	"	`variationId`,	"
	"	`runRangeId`,	"
	"	`eventRangeId`,	"
	"	`constantSetId`,"
	"	`comment`)		"
	"	VALUES	(NULL, "		//modified update 
	"  %i, "					//  #{variationId: INT}
	"	%i, "					//  #{runRangeId: INT},		  
	"	NULL,"					//	#{eventRangeId: INT},
	"	LAST_INSERT_ID(), "		//	#{constantSetId: INT},	
	"	%s "					//	#{comment: TEXT}
	");	";
	query = DStringUtils::Format(query.c_str(), 
		assignment->GetVariation()->GetId(), 
		assignment->GetRunRange()->GetId(),
		PrepareCommentForInsert(assignment->GetComment().c_str()).c_str());
	//query...
	if(!QueryInsert(query))
	{
		//TODO report error
		QueryCustom("ROLLBACK;"); //rollback transaction doesnt matter will it happen or not but we should try
		return false;
	}
	assignment->SetId(static_cast<dbkey_t>(mLastInsertedId));
	
	//adjust number in data table
	query = DStringUtils::Format("UPDATE typeTables SET nAssignments=nAssignments+1 WHERE id='%i';", table->GetId());
	QueryUpdate(query);
	
	//commit changes
	if(!QueryCustom("COMMIT;"))
	{	
		return false; 
	}

	//just log this wicked action
	AddLogRecord("assignments;constantSets;",
		DStringUtils::Format("assignments_%i;constantSets_%i", assignment->GetId(), assignment->GetDataVaultId()),
		DStringUtils::Format("Add assignment to %s", assignment->GetTypeTable()->GetName().c_str()),
		DStringUtils::Format("Add assignment to %s,\n comments: %s", assignment->GetTypeTable()->GetName().c_str(), table->GetComment().c_str()));
	return true;
}

DAssignment* ccdb::DMySQLDataProvider::CreateAssignment(const std::vector<std::vector<std::string> >& data, const std::string& path, int runMin, int runMax, const std::string& variationName, const std::string& comments)
{

	/* Creates Assignment using related object.
	* Validation:
	* If no such run range found, the new will be created (with no name)
	* No action will be done (and NULL will be returned):
	* 
	* -- If no type table with such path exists
	* -- If data is inconsistent with columns number and rows number
	* -- If no variation with such name found */
	ClearErrors(); //Clear error in function that can produce new ones
	
	DVariation* variation = GetVariation(variationName);
	if(variation == NULL)
	{
		 //TODO error message
		Error(CCDB_ERROR_VARIATION_INVALID,"DMySQLDataProvider::CreateAssignment", "Variation is NULL or has improper ID");
		return NULL;
	}
	 
	DConstantsTypeTable* table=GetConstantsTypeTable(path, true);
	if(!table)
	{
		//TODO error message
		Error(CCDB_ERROR_NO_TYPETABLE,"DMySQLDataProvider::CreateAssignment", "Type table is NULL or has improper ID");
		return NULL;
	}
	 
	//check that we have right rows number
	if(data.size()!= table->GetNRows())
	{
		 //error message
		Error(CCDB_ERROR_TABLE_NO_ROWS,"DMySQLDataProvider::CreateAssignment", "For given table number of rows is zero");
		return NULL;
	}

	//fill data blob vector
	vector<string> vectorBlob;
	for (size_t rowIter=0; rowIter<data.size(); rowIter++)
	{
		const vector<string> &row = data[rowIter];
		if(row.size() != table->GetNColumns())
		{
			//TODO error handle
			 //TODO error message
			Error(CCDB_ERROR_DATA_INCONSISTANT,"DMySQLDataProvider::CreateAssignment", "Number of columns in rows is inconsistant");
			return NULL;
		}

		for (int i=0; i<row.size(); i++)
		{
			vectorBlob.push_back(row[i]);
		}
	}
	
	//last one we need is a run range
	DRunRange * runRange = GetOrCreateRunRange(runMin, runMax, "", comments);
	if(runRange == NULL)
	{
		//TODO report cannot creat runrange
		Error(CCDB_ERROR_RUNRANGE_INVALID,"DMySQLDataProvider::CreateAssignment", "Can not get or create run range");
		return NULL;
	}

	DAssignment * assignment=new DAssignment(this, this);
	assignment->SetRawData(DAssignment::VectorToBlob(vectorBlob));
	
	assignment->SetVariation(variation);
	assignment->BeOwner(variation);

	assignment->SetRunRange(runRange);
	assignment->BeOwner(runRange);

	assignment->SetTypeTable(table);	//set this table
	assignment->BeOwner(table);			//new table should be owned by assignment

	assignment->SetComment(DStringUtils::Encode(comments));

	if(CreateAssignment(assignment))
	{
		return assignment;
	}
	else 
	{
		delete assignment;
		return NULL;
	}
}

DAssignment* ccdb::DMySQLDataProvider::GetAssignmentFull( int run, const string& path, const string& variation )
{
	if(!CheckConnection("DMySQLDataProvider::GetAssignmentFull(int run, cconst string& path, const string& variation")) return NULL;
	vector<DAssignment *> assigments;
	if(!GetAssignments(assigments, path, run,run, "", variation, 0, 0, 0, 1, 0))
	{
		return NULL;
	}
	
	if(assigments.size()<=0) return NULL;
	
	return *assigments.begin();
	
}

DAssignment* ccdb::DMySQLDataProvider::GetAssignmentFull( int run, const string& path,int version, const string& variation/*= "default"*/)
{

	if(!CheckConnection("DMySQLDataProvider::GetAssignmentFull( int run, const char* path, const char* variation, int version /*= -1*/ )")) return NULL;
	
	//get table! 
	vector<DAssignment *> assigments;
	if(!GetAssignments(assigments, path, run,run, "", variation, 0, 0, 1, 1, version))
	{
		return NULL;
	}

	if(assigments.size()<=0) return NULL;

	return *assigments.begin();
}


bool ccdb::DMySQLDataProvider::GetAssignments( vector<DAssignment *> &assingments,const string& path, int runMin, int runMax, const string& runRangeName, const string& variation, time_t beginTime, time_t endTime, int sortBy/*=0*/,  int take/*=0*/, int startWith/*=0*/ )
{
	ClearErrors(); //Clear error in function that can produce new ones

	if(!CheckConnection("DMySQLDataProvider::GetAssignments( ... )")) return false;

	//get table! 
	DConstantsTypeTable *table = GetConstantsTypeTable(path, true);
	if(!table)
	{
		//TODO report error
		Error(CCDB_ERROR_NO_TYPETABLE,"DMySQLDataProvider::GetAssignments", "Type table was not found");
		return false;
	}

	//Ok, lets cleanup result list
	if(assingments.size()>0)
	{
		vector<DAssignment *>::iterator iter = assingments.begin();
		while(iter != assingments.end())
		{
			DAssignment *obj = *iter;
			if(IsOwner(obj)) delete obj;	//delete objects if this provider is owner
			iter++;
		}
	}
	assingments.clear();

	//run range handle
	string runRangeWhere(""); //Where clause for run range
	if(runRangeName != "")
	{
		runRangeWhere = DStringUtils::Format(" AND `runRanges`.`name` = \"%s\" ", runRangeName.c_str());
	}
	else if(runMax!=0 || runMin!=0)
	{
		runRangeWhere = DStringUtils::Format(" AND `runRanges`.`runMin` <= '%i' AND `runRanges`.`runMax` >= '%i' ", runMin, runMax);
	}

	//variation handle
	string variationWhere("");
	if(variation != "")
	{
		variationWhere.assign(DStringUtils::Format(" AND `variations`.`name`=\"%s\" ", variation.c_str()));
	}

	//time handle 
	string timeWhere("");
	if(beginTime!=0 || endTime!=0)
	{
		timeWhere.assign(DStringUtils::Format(" AND `asCreated` >= '%i' AND `asCreated` <= '%i' ", beginTime, endTime));
	}

	//limits handle 
	string limitInsertion = PrepareLimitInsertion(take, startWith);

	//sort order
	string orderByInsertion(" ORDER BY `assignments`.`created` DESC ");
	if(sortBy == 1)
	{
		orderByInsertion.assign(" ORDER BY `assignments`.`created` ASC ");
	}


	//ok now we must build our mighty query...
	string query=
	/*fieldN*/  " SELECT "
	/*00*/  " `assignments`.`id` AS `asId`,	"
	/*01*/  " UNIX_TIMESTAMP(`assignments`.`created`) as `asCreated`,"
	/*02*/  " UNIX_TIMESTAMP(`assignments`.`modified`) as `asModified`,	"
	/*03*/  " `assignments`.`comment` as `asComment`, "
	/*04*/  " `constantSets`.`id` AS `constId`, "
	/*05*/  " `constantSets`.`vault` AS `blob`, "
	/*06*/  " `runRanges`.`id`   AS `rrId`, "
	/*07*/  " UNIX_TIMESTAMP(`runRanges`.`created`) as `rrCreated`,"
	/*08*/  " UNIX_TIMESTAMP(`runRanges`.`modified`) as `rrModified`,	"
	/*09*/  " `runRanges`.`name`   AS `rrName`, "
	/*10*/  " `runRanges`.`runMin`   AS `rrMin`, "
	/*11*/  " `runRanges`.`runMax`   AS `runMax`, "
	/*12*/  " `runRanges`.`comment` as `rrComment`, "
	/*13*/  " `variations`.`id`  AS `varId`, "
	/*14*/  " UNIX_TIMESTAMP(`variations`.`created`) as `varCreated`,"
	/*15*/  " UNIX_TIMESTAMP(`variations`.`modified`) as `varModified`,	"
	/*16*/  " `variations`.`name` AS `varName`, "
	/*17*/  " `variations`.`comment`  AS `varComment` "
		    " FROM `runRanges` "
		    " INNER JOIN `assignments` ON `assignments`.`runRangeId`= `runRanges`.`id`"
		    " INNER JOIN `variations` ON `assignments`.`variationId`= `variations`.`id`	"
		    " INNER JOIN `constantSets` ON `assignments`.`constantSetId` = `constantSets`.`id`"
		    " INNER JOIN `typeTables` ON `constantSets`.`constantTypeId` = `typeTables`.`id`"
		    " WHERE  `typeTables`.`id` = '%i' %s %s %s %s %s";


	query=DStringUtils::Format(query.c_str(), table->GetId(), runRangeWhere.c_str(), variationWhere.c_str(), timeWhere.c_str(), orderByInsertion.c_str(), limitInsertion.c_str());
	//query this
	if(!QuerySelect(query))
	{
		//TODO report error
		return NULL;
	}

	//Ok! We querried our run range! lets catch it! 
	while(FetchRow())
	{
		assingments.push_back(FetchAssignment(table));
		
	}

	FreeMySQLResult();
	
	return true;
}

bool ccdb::DMySQLDataProvider::GetAssignments(vector<DAssignment*>& assingments, const string& path, int run, const string& variation, time_t date, int take, int startWith)
{
	return GetAssignments(assingments, path, run, run,"",variation, 0, date, take, startWith);
}

vector<DAssignment *> ccdb::DMySQLDataProvider::GetAssignments( const string& path, int run, const string& variation/*=""*/, time_t date/*=0*/, int take/*=0*/, int startWith/*=0*/ )
{
	vector<DAssignment *> assingments;
	GetAssignments(assingments, path, run,variation, date, take, startWith);
	return assingments;
}

bool ccdb::DMySQLDataProvider::GetAssignments( vector<DAssignment *> &assingments,const string& path, const string& runName, const string& variation/*=""*/, time_t date/*=0*/, int take/*=0*/, int startWith/*=0*/ )
{
	return GetAssignments(assingments, path, 0, 0,runName,variation, 0, date, take, startWith);
}

vector<DAssignment *> ccdb::DMySQLDataProvider::GetAssignments( const string& path, const string& runName, const string& variation/*=""*/, time_t date/*=0*/, int take/*=0*/, int startWith/*=0*/ )
{
	vector<DAssignment *> assingments;
	GetAssignments(assingments, path, runName,variation, date, take, startWith);
	return assingments;
}

bool ccdb::DMySQLDataProvider::UpdateAssignment(DAssignment* assignment)
{
	Error(
		CCDB_ERROR_NOT_IMPLEMENTED, 
		"DMySQLDataProvider::UpdateAssignment(DAssignment* assignment)",
		"Method is not implemented");
	return false;
}

bool ccdb::DMySQLDataProvider::DeleteAssignment(DAssignment* assignment)
{
	ClearErrors(); //Clear error in function that can produce new ones

	//Check primary ID
	if(!assignment->GetId())
	{
		//todo error
		Error(CCDB_ERROR_ASSIGMENT_INVALID_ID,"DMySQLDataProvider::DeleteAssignment", "!assignment->GetId()");
		return false;
	}
	
	//Check data vault ID
	if(!assignment->GetDataVaultId())
	{
		//todo error 
		Error(CCDB_ERROR_ASSIGMENT_INVALID_ID,"DMySQLDataProvider::DeleteAssignment", "!assignment->GetDataVaultId()");
		return false;
	}
	
	//Check that all is OK with table
	if(!assignment->GetTypeTable() || !assignment->GetTypeTable()->GetId())
	{
		//todo error 
		Error(CCDB_ERROR_NO_TYPETABLE,"DMySQLDataProvider::DeleteAssignment", "Type table is null or has improper id");
		return false;
	}
	
	//start query, lock tables, make transaction imune;
	if(!QueryCustom("START TRANSACTION;"))
	{
		return false; 
	}
	
	//add constants
	string query = DStringUtils::Format(" DELETE FROM  constantSets WHERE constantSets.id = '%i% ;",assignment->GetDataVaultId());
	
	//query...
	if(!QueryDelete(query))
	{
		//TODO report error
		QueryCustom("ROLLBACK;"); //rollback transaction doesnt matter will it happen or not but we should try
		return false;
	}
	
	query = DStringUtils::Format(" DELETE FROM  assignments WHERE assignments.id = '%i% ;",assignment->GetId());
	
	//query...
	if(!QueryDelete(query))
	{
		//TODO report error
		QueryCustom("ROLLBACK;"); //rollback transaction doesnt matter will it happen or not but we should try
		return false;
	}
	
	//adjust number in data table
	query = DStringUtils::Format("UPDATE typeTables SET nAssignments=nAssignments-1 WHERE id='%i';", assignment->GetTypeTable()->GetId());
	QueryUpdate(query);
	
	//commit changes
	if(!QueryCustom("COMMIT;"))
	{
		return false; 
	}
	
	return true;
}

bool ccdb::DMySQLDataProvider::FillAssignment(DAssignment* assignment)
{
	ClearErrors(); //Clear error in function that can produce new ones
	if(assignment == NULL || !assignment->GetId())
	{
		//todo report errors
		Error(CCDB_ERROR_ASSIGMENT_INVALID,"DMySQLDataProvider::FillAssignment", "ASSIGnMENt is NULL or has improper ID so update operations cant be done");
		return false;
	}
	
	//ok now we must build our mighty query...
	string query=
	/*fieldN*/  " SELECT "
	/*00*/  " `assignments`.`id` AS `asId`,	"
	/*01*/  " UNIX_TIMESTAMP(`assignments`.`created`) as `asCreated`,"
	/*02*/  " UNIX_TIMESTAMP(`assignments`.`modified`) as `asModified`,	"
	/*03*/  " `assignments`.`comment` as `asComment`, "
	/*04*/  " `constantSets`.`id` AS `constId`, "
	/*05*/  " `constantSets`.`vault` AS `blob`, "
	/*06*/  " `runRanges`.`id`   AS `rrId`, "
	/*07*/  " UNIX_TIMESTAMP(`runRanges`.`created`) as `rrCreated`,"
	/*08*/  " UNIX_TIMESTAMP(`runRanges`.`modified`) as `rrModified`,	"
	/*09*/  " `runRanges`.`name`   AS `rrName`, "
	/*10*/  " `runRanges`.`runMin`   AS `rrMin`, "
	/*11*/  " `runRanges`.`runMax`   AS `runMax`, "
	/*12*/  " `runRanges`.`comment` as `rrComment`, "
	/*13*/  " `variations`.`id`  AS `varId`, "
	/*14*/  " UNIX_TIMESTAMP(`variations`.`created`) as `varCreated`,"
	/*15*/  " UNIX_TIMESTAMP(`variations`.`modified`) as `varModified`,	"
	/*16*/  " `variations`.`name` AS `varName`, "
	/*17*/  " `variations`.`comment`  AS `varComment`, "
	/*18*/  " `typeTables`.`name` AS `typeTableName`, "
	/*19*/  " `directories`.`id` AS `dirId` "
		    " FROM `runRanges` "
		    " INNER JOIN `assignments` ON `assignments`.`runRangeId`= `runRanges`.`id`"
		    " INNER JOIN `variations` ON `assignments`.`variationId`= `variations`.`id`	"
		    " INNER JOIN `constantSets` ON `assignments`.`constantSetId` = `constantSets`.`id`"
		    " INNER JOIN `typeTables` ON `constantSets`.`constantTypeId` = `typeTables`.`id`"
			" INNER JOIN `directories` ON `typeTables`.`directoryId` = `directories`.`id`"
		    " WHERE  `assignments`.`id` = '%i'";
	
	
	query=DStringUtils::Format(query.c_str(), assignment->GetId());
	//cout << query<<endl;
	//query this
	if(!QuerySelect(query))
	{
		//TODO report error
		cout<<"!QuerySelect(query)"<<endl;
		return false;
	}
	
	if(!FetchRow())
	{
		//TODO report error
		cout<<"no assignment selected!"<<endl;
		return false;
	}
	
	//fetch readed row
	FetchAssignment(assignment, assignment->GetTypeTable());
	
	//Ok! Here is a tricky moment. We should load a constant type table 
	//but we have an API that allows us to load such directories by name
	//The problem that we should know a full path of it. 
	//So we pulled a directory ID and data table name. 
	// We have a list of directories by their ID thus we will know a full path
	// of type table. 
	// lets make this plan work
	
	string typeTableName = ReadString(18);
	dbkey_t directoryId = ReadIndex(19);
	
	//Mayby we need to update our directories?
	UpdateDirectoriesIfNeeded();

	if(mDirectoriesById.find(directoryId) == mDirectoriesById.end())
	{
		//todo report errors
		Error(CCDB_ERROR,"DMySQLDataProvider::FillAssignment", "Cannot find directory locally by ID taken from database");
		return false;
	}

	DDirectory *parent = mDirectoriesById[directoryId];
	
	DConstantsTypeTable * table = GetConstantsTypeTable(typeTableName, parent, true);
	if(!table)
	{
		Error(CCDB_ERROR_NO_TYPETABLE,"DMySQLDataProvider::FillAssignment", "Type table was not loaded");
		return false;
	}

	assignment->SetTypeTable(table);
	table->SetOwner(assignment);

	FreeMySQLResult();
	
	return true;
}

DAssignment* ccdb::DMySQLDataProvider::FetchAssignment(DConstantsTypeTable* table)
{
	DAssignment * assignment = new DAssignment(this, this); //it is our victim

	FetchAssignment(assignment, table);

	//take it!
	return assignment;
}


void ccdb::DMySQLDataProvider::FetchAssignment(DAssignment* assignment, DConstantsTypeTable* table)
{
	//ok now we must fetch our mighty query...
	assignment->SetId(ReadIndex(0));				/*00  " `assignments`.`id` AS `asId`,	"*/
	assignment->SetCreatedTime(ReadUnixTime(1));	/*01  " UNIX_TIMESTAMP(`assignments`.`created`) as `asCreated`,"*/
	assignment->SetModifiedTime(ReadUnixTime(2));	/*02  " UNIX_TIMESTAMP(`assignments`.`modified`) as `asModified`,	"*/
	assignment->SetComment(ReadString(3));			/*03  " `assignments`.`comment) as `asComment`,	"					 */
	assignment->SetDataVaultId(ReadIndex(4));		/*04  " `constantSets`.`id` AS `constId`, "							 */
	assignment->SetRawData(ReadString(5));			/*05  " `constantSets`.`vault` AS `blob`, "							 */
	
	DRunRange * runRange = new DRunRange(assignment, this);	
	runRange->SetId(ReadIndex(6));					/*06  " `runRanges`.`id`   AS `rrId`, "	*/
	runRange->SetCreatedTime(ReadUnixTime(7));		/*07  " UNIX_TIMESTAMP(`runRanges`.`created`) as `rrCreated`,"*/
	runRange->SetModifiedTime(ReadUnixTime(8));		/*08  " UNIX_TIMESTAMP(`runRanges`.`modified`) as `rrModified`,	"*/
	runRange->SetName(ReadString(9));				/*09  " `runRanges`.`name`   AS `rrName`, "		*/
	runRange->SetMin(ReadInt(10));					/*10  " `runRanges`.`runMin`   AS `rrMin`, "	*/
	runRange->SetMax(ReadInt(11));					/*11  " `runRanges`.`runMax`   AS `runMax`, "	*/
	runRange->SetComment(ReadString(12));			/*12  " `runRanges`.`comment` as `rrComment`, "	*/
	
	DVariation * variation = new DVariation(assignment, this);
	variation->SetId(ReadIndex(13));				/*13  " `variations`.`id`  AS `varId`, */
	variation->SetCreatedTime(ReadUnixTime(14));	/*14  " UNIX_TIMESTAMP(`variations`.`created`) as `varCreated`,"	 */
	variation->SetModifiedTime(ReadUnixTime(15));	/*15  " UNIX_TIMESTAMP(`variations`.`modified`) as `varModified`,	 */
	variation->SetName(ReadString(16));			/*16  " `variations`.`name` AS `varName` "*/
	variation->SetComment(DStringUtils::Decode(ReadString(12)));		/*17  " `variations`.`comment`  AS `varComment` "*/

	//compose objects
	assignment->SetRunRange(runRange);
	assignment->SetVariation(variation);
	assignment->SetTypeTable(table);
	if(IsOwner(table)) table->SetOwner(assignment);
}



bool ccdb::DMySQLDataProvider::LoadDirectories()
{
	//
	if(IsConnected())
	{
		if(!QuerySelect("SELECT `id`, `name`, `parentId`, UNIX_TIMESTAMP(`directories`.`modified`) as `updateTime`, `comment` FROM `directories`"))
		{
			//TODO: report error
			return false;
		}

		//clear diretory arrays
		mDirectories.clear();
		mDirectoriesById.clear();

		//clear root directory (delete all directory structure objects)
		mRootDir->DisposeSubdirectories(); 
		mRootDir->SetFullPath("/");

		//Ok! We querryed our directories! lets catch them! 
		while(FetchRow())
		{
			DDirectory *dir = new DDirectory(this, this);
			dir->SetId(ReadIndex(0));					// `id`, 
			dir->SetName(ReadString(1));			// `name`, 
			dir->SetParentId(ReadInt(2));			// `parentId`, 
			dir->SetModifiedTime(ReadUnixTime(3));	// UNIX_TIMESTAMP(`directories`.`updateTime`) as `updateTime`, 
			dir->SetComment(ReadString(4));			// `comment`

			mDirectories.push_back(dir);
			mDirectoriesById[dir->GetId()] = dir;
		}

		BuildDirectoryDependences(); //

		mDirsAreLoaded=true;
	}
	return false;
}

void ccdb::DMySQLDataProvider::BuildDirectoryDependences()
{
	// this method is supposed to be called after new directories are loaded, but dont have hierarchical structure

	//clear the full path dictionary
	mDirectoriesByFullPath.clear();
	mDirectoriesByFullPath[mRootDir->GetFullPath()] = mRootDir;

	//begin loop through the directories
	vector<DDirectory *>::iterator dirIter = mDirectories.begin();
	for(;dirIter < mDirectories.end(); dirIter++)
	{
		// retrieve parent id 
		// and check if it have parent directory
		DDirectory *dir = *dirIter; //(*dirIter) cool it is C++, baby

		if(dir->GetParentId() >0)
		{
			//this directory must have a parent! so now search it
			map<int,DDirectory *>::iterator parentDirIter = mDirectoriesById.find(dir->GetParentId()) ;
			if(parentDirIter!=mDirectoriesById.end())
			{
				//We found parent
				parentDirIter->second->AddSubdirectory(*dirIter);
			}
			else
			{
				//TODO : ADD error, parent Id not found
				Error(CCDB_ERROR_NO_PARENT_DIRECTORY,"DMySQLDataProvider::BuildDirectoryDependences", "Parent directory with wrong id");
				continue; //we have to stop operate this directory...
			}
		}
		else
		{
			// this directory doesn't have parent 
			// (means it doesn't have parentId so it lays in root directory)
			// so we place it to root directory
			mRootDir->AddSubdirectory(*dirIter);
		}

		//creating full path for this directory
		string fullpath = DStringUtils::CombinePath(dir->GetParentDirectory()->GetFullPath(), dir->GetName());
		dir->SetFullPath(fullpath);


		//add to our full path map
		mDirectoriesByFullPath[dir->GetFullPath()] = dir;
	}
}

DDirectory*const ccdb::DMySQLDataProvider::GetRootDirectory()
{
	UpdateDirectoriesIfNeeded();
	return mRootDir;
}

bool ccdb::DMySQLDataProvider::CheckDirectoryListActual()
{
	//TODO: method should check the database if the directories was updated in DB. Now it only checks if directories were loaded

	if(!mDirsAreLoaded) return false; //directories are not loaded

	return !mNeedCheckDirectoriesUpdate; 
}

bool ccdb::DMySQLDataProvider::UpdateDirectoriesIfNeeded()
{	
	//Logic to check directories...
	//if(!CheckDirectoryListActual()) return LoadDirectories(); //TODO check braking links
	//repairing it
	if(!mDirsAreLoaded) return LoadDirectories();
	return true;
}

bool ccdb::DMySQLDataProvider::SearchDirectories( vector<DDirectory *>& resultDirectories, const string& searchPattern, const string& parentPath/*=""*/,  int take/*=0*/, int startWith/*=0*/ )
{	
	UpdateDirectoriesIfNeeded(); //do we need to update directories?

	resultDirectories.clear();

	// in MYSQL compared to wildcards % is * and _ is 
	// convert it. 
	string likePattern = WilcardsToLike(searchPattern);
	
	//do we need to search only in specific directory?
	string parentAddon(""); //this is addon to query indicates this
	if(parentPath!="")
	{	//we should care about parent path
		
		//If parent directory is "/" this should work too, because it have an id=0
		//and tabeles in db wich doesnt have parents should have parentId=0
		
		DDirectory *parentDir;
		if(parentDir = GetDirectory(parentPath.c_str()))
		{
			parentAddon = DStringUtils::Format(" AND `parentId` = '%i'", parentDir->GetId());
		}
		else
		{
			//request was made for directory that doestn exits
			//TODO place warning or not?
			return false;
		}
	}

	string limitAddon = PrepareLimitInsertion(take, startWith);

	//combine query
	string query = DStringUtils::Format("SELECT `id` FROM `directories` WHERE name LIKE \"%s\" %s %s;",
		likePattern.c_str(), parentAddon.c_str(), limitAddon.c_str());
	
	//do query!
	if(!QuerySelect(query))
	{
		return false;
	}
	
	//Ok! We queried our directories! lets catch them! 
	while(FetchRow())
	{
		dbkey_t id = ReadIndex(0); //read db index key

		//search for such index
		map<dbkey_t,DDirectory *>::iterator dirIter = mDirectoriesById.find(id);
		if(dirIter != mDirectoriesById.end())
		{
			resultDirectories.push_back(dirIter->second);
		}
		else
		{
			//TODO it is some error situation! It cant happend!
		}
	}

	FreeMySQLResult();

	return true;
}

vector<DDirectory *> ccdb::DMySQLDataProvider::SearchDirectories( const string& searchPattern, const string& parentPath/*=""*/, int startWith/*=0*/, int select/*=0*/ )
{
	vector<DDirectory *> result;
	SearchDirectories(result, searchPattern, parentPath, startWith, select);
	return result;
}

std::string ccdb::DMySQLDataProvider::WilcardsToLike( const string& str )
{	
	//MySQL - wildcards
	//% - *
	//_ - ?

	//encode underscores
	string result = DStringUtils::Replace("_", "\\_", str);

	//replace ? to _
	DStringUtils::Replace("?","_",result, result);

	//replace * to %
	DStringUtils::Replace("*","%",result, result);

	return result;
}

std::string ccdb::DMySQLDataProvider::PrepareCommentForInsert( const string& comment )
{
	// The idea is that we have to place 
	// (..., NULL) in INSERT queries if comment is NULL
	// and (..., "<content>") if it is not NULL
	// So this function returns string containing NULL or \"<comment content>\"
	

	if(comment.length() == 0)
	{
		// we put empty comments as NULL
		//because comments uses "TEXT" field that takes additional size
		// if text field in DB is NULL it will be read as "" by ReadString()
		// so it is safe to do this 
		
		return string("NULL");
	}
	else
	{
		string commentIns ="\"";
		commentIns.append(comment);
		commentIns.append("\"");
		return commentIns;
	}

}



#pragma region MySQL_Field_Operations

bool ccdb::DMySQLDataProvider::IsNullOrUnreadable( int fieldNum )
{
	// Checks if there is a value with this fieldNum index (reports error in such case)
	// and if it is not null (just returns false in this case)

	if(mReturnedFieldsNum<=fieldNum)
	{
		//Add error, we have less fields than fieldNum
		Error(CCDB_WARNING_MYSQL_FIELD_NUM,"DMySQLDataProvider::IsNullOrUnreadable", "we have less fields than fieldNum");
		return true;
	}

	if(mRow[fieldNum]==NULL) 
	{	
		return true;
	}
	//fine! 
	return false;
}

int ccdb::DMySQLDataProvider::ReadInt( int fieldNum )
{	
	if(IsNullOrUnreadable(fieldNum)) return 0;

	return atoi(mRow[fieldNum]); //ugly isn't it?
}

unsigned int ccdb::DMySQLDataProvider::ReadUInt( int fieldNum )
{	
	if(IsNullOrUnreadable(fieldNum)) return 0;

	return static_cast<unsigned int>(atoi(mRow[fieldNum])); //ugly isn't it?
}

long ccdb::DMySQLDataProvider::ReadLong( int fieldNum )
{
	if(IsNullOrUnreadable(fieldNum)) return 0;

	return atol(mRow[fieldNum]); //ugly isn't it?
}

unsigned long ccdb::DMySQLDataProvider::ReadULong( int fieldNum )
{
	if(IsNullOrUnreadable(fieldNum)) return 0;

	return static_cast<unsigned long>(atol(mRow[fieldNum])); //ugly isn't it?
}

dbkey_t ccdb::DMySQLDataProvider::ReadIndex( int fieldNum )
{
	if(IsNullOrUnreadable(fieldNum)) return 0;

	return static_cast<dbkey_t>(atol(mRow[fieldNum])); //ugly isn't it?
}

bool ccdb::DMySQLDataProvider::ReadBool( int fieldNum )
{
	if(IsNullOrUnreadable(fieldNum)) return false;

	return static_cast<bool>(atoi(mRow[fieldNum])!=0); //ugly isn't it?

}

double ccdb::DMySQLDataProvider::ReadDouble( int fieldNum )
{
	if(IsNullOrUnreadable(fieldNum)) return 0;

	return atof(mRow[fieldNum]); //ugly isn't it?
}

std::string ccdb::DMySQLDataProvider::ReadString( int fieldNum )
{
	if(IsNullOrUnreadable(fieldNum)) return string("");
	return string(mRow[fieldNum]);
}


time_t ccdb::DMySQLDataProvider::ReadUnixTime( int fieldNum )
{	
	return static_cast<time_t>(ReadULong(fieldNum));
}

#pragma endregion MySQL_Field_Operations


#pragma region Queries

bool ccdb::DMySQLDataProvider::QuerySelect(const char* query)
{
	if(!CheckConnection("DMySQLDataProvider::QuerySelect")) return false;
	
	//do we have some results we need to free?
	if(mResult!=NULL)
	{		
		FreeMySQLResult();
	}

	//query
	if(mysql_query(mMySQLHnd, query))
	{
		string errStr = ComposeMySQLError("mysql_query()"); errStr.append("\n Query: "); errStr.append(query);
		Error(CCDB_ERROR_MYSQL_SELECT,"ccdb::DMySQLDataProvider::QuerySelect()",errStr.c_str());
		return false;
	}

	//get results
	mResult = mysql_store_result(mMySQLHnd);

	if(!mResult)
	{
		string errStr = ComposeMySQLError("mysql_query()"); errStr.append("\n Query: "); errStr.append(query);
		Error(CCDB_ERROR_MYSQL_SELECT,"ccdb::DMySQLDataProvider::QuerySelect()",errStr.c_str());

			
		mReturnedRowsNum = 0;
		mReturnedFieldsNum = 0;
		return false;
	}

	//a rows number?
	mReturnedRowsNum = mysql_num_rows(mResult);

	//a fields number?
	mReturnedFieldsNum = mysql_num_fields(mResult);

	return true;
	
}

bool ccdb::DMySQLDataProvider::QuerySelect(const string& query )
{
	return QuerySelect(query.c_str());
}


bool ccdb::DMySQLDataProvider::QueryInsert( const char* query )
{
	if(!CheckConnection("DMySQLDataProvider::QueryCustom")) return false;
		
	//do we have some results we need to free?
	if(mResult!=NULL)
	{	
		FreeMySQLResult();
	}

	//query
	if(mysql_query(mMySQLHnd, query))
	{
		string errStr = ComposeMySQLError("mysql_query()"); errStr.append("\n Query: "); errStr.append(query);
		Error(CCDB_ERROR_MYSQL_SELECT,"ccdb::DMySQLDataProvider::QueryInsert()",errStr.c_str());
		return false;
	}

	//get last inserted id
	if ((mResult = mysql_store_result(mMySQLHnd)) == 0 &&
		mysql_field_count(mMySQLHnd) == 0 &&
		mysql_insert_id(mMySQLHnd) != 0)
	{
		mLastInsertedId = mysql_insert_id(mMySQLHnd);
		return true;
	}

	return false;	
}

bool ccdb::DMySQLDataProvider::QueryInsert( const string& query )
{
	return QueryInsert(query.c_str());
}


bool ccdb::DMySQLDataProvider::QueryUpdate( const char* query )
{
	if(!CheckConnection("DMySQLDataProvider::QueryCustom")) return false;
	
	//do we have some results we need to free?
	if(mResult!=NULL)
	{	
		FreeMySQLResult();
	}

	//query
	if(mysql_query(mMySQLHnd, query))
	{
		//TODO: error report
		string errStr = ComposeMySQLError("mysql_query()"); errStr.append("\n Query: "); errStr.append(query);
		Error(CCDB_ERROR_MYSQL_UPDATE,"ccdb::DMySQLDataProvider::QueryUpdate()",errStr.c_str());
		return false;
	}

	mReturnedAffectedRows = mysql_affected_rows(mMySQLHnd);

	return mReturnedAffectedRows>0;
}

bool ccdb::DMySQLDataProvider::QueryUpdate( const string& query ) /*/Do "Update" queries */
{
	return QueryUpdate(query.c_str());
}

bool ccdb::DMySQLDataProvider::QueryDelete( const char* query )
{
	if(!CheckConnection("DMySQLDataProvider::QueryCustom")) return false;
	
	//do we have some results we need to free?
	if(mResult!=NULL)
	{	
		FreeMySQLResult();
	}
	//query
	if(mysql_query(mMySQLHnd, query))
	{
		//TODO: error report
		string errStr = ComposeMySQLError("mysql_query()"); errStr.append("\n Query: "); errStr.append(query);
		Error(CCDB_ERROR_MYSQL_DELETE,"ccdb::DMySQLDataProvider::QueryDelete()",errStr.c_str());
		return false;
	}

	mReturnedAffectedRows = mysql_affected_rows(mMySQLHnd);

	return true;
}

bool ccdb::DMySQLDataProvider::QueryDelete( const string& query )
{
	return QueryDelete(query.c_str());
}

bool ccdb::DMySQLDataProvider::QueryCustom( const string& query )
{
	if(!CheckConnection("DMySQLDataProvider::QueryCustom")) return false;

	//do we have some results we need to free?
	if(mResult!=NULL)
	{	
		FreeMySQLResult();
	}
	//query
	if(mysql_query(mMySQLHnd, query.c_str()))
	{
		string errStr = ComposeMySQLError("mysql_query()"); errStr.append("\n Query: "); errStr.append(query);
		Error(CCDB_ERROR_MYSQL_CUSTOM_QUERY,"ccdb::DMySQLDataProvider::QueryCustom( string query )",errStr.c_str());
		return false;
	}
	return true;
}



#pragma endregion


#pragma region Fetch_free_and_other_MySQL_operations

bool ccdb::DMySQLDataProvider::FetchRow()
{	
	if(mRow = mysql_fetch_row(mResult)) return true;
	return false;
}

void ccdb::DMySQLDataProvider::FreeMySQLResult()
{
	mysql_free_result(mResult);
	mResult = NULL;
}

std::string ccdb::DMySQLDataProvider::ComposeMySQLError(std::string mySqlFunctionName)
{
	string mysqlErr=DStringUtils::Format("%s failed:\nError %u (%s)\n",mySqlFunctionName.c_str(), mysql_errno(mMySQLHnd), mysql_error (mMySQLHnd));
	return mysqlErr;
}
#pragma endregion Fetch_free_and_other_MySQL_operations


void ccdb::DMySQLDataProvider::AddLogRecord( string userName, string affectedTables, string affectedIds, string shortDescription, string fullDescription )
{
	int id = GetUserId(userName);	

	string query = "INSERT INTO `logs` "
		" (`affectedTables`, `affectedIds`, `authorId`, `description`,  `fullDescription`) VALUES "
		" (    \"%s\"      ,      \"%s\"  , 	 %i   ,    \"%s\"    ,        \"%s\"     ); ";
	query = DStringUtils::Format(query.c_str(), affectedTables.c_str(), affectedIds.c_str(), id, shortDescription.c_str(), fullDescription.c_str());
	
	QueryInsert(query);
	
}

void ccdb::DMySQLDataProvider::AddLogRecord( string affectedTables, string affectedIds, string shortDescription, string fullDescription )
{
	AddLogRecord(mLogUserName, affectedTables, affectedIds, shortDescription, fullDescription);
}

dbkey_t ccdb::DMySQLDataProvider::GetUserId( string userName )
{
	if(userName == "" || !ValidateName(userName))
	{
		return 1; //anonymous id
	}

	string query = DStringUtils::Format("SELECT `id` FROM `authors` WHERE `name` = \"%s\" LIMIT 0,1", userName.c_str());
	if(!QuerySelect(query))
	{
		return 1; //anonymous id
	}


	if(!FetchRow())
	{
		return 1; //anonymous id
	}

	dbkey_t id = ReadIndex(0);

	FreeMySQLResult();

	if(id<=0) return 1;
	return id;
}


std::string ccdb::DMySQLDataProvider::PrepareLimitInsertion(  int take/*=0*/, int startWith/*=0*/ )
{
	if(startWith != 0 && take != 0) return DStringUtils::Format(" LIMIT %i, %i ", startWith, take);
	if(startWith != 0 && take == 0) return DStringUtils::Format(" LIMIT %i, %i ", startWith, INFINITE_RUN);
	if(startWith == 0 && take != 0) return DStringUtils::Format(" LIMIT %i ", take);
	
	return string(); //No LIMIT at all, if run point is here it corresponds to if(startWith == 0 && take ==0 )

}


int ccdb::DMySQLDataProvider::CountConstantsTypeTables(DDirectory *dir)
{
	/**
	 * @brief This function counts number of type tables for a given directory 
	 * @param [in] directory to look tables in
	 * @return number of tables to return
	 */
	return 0;
}
