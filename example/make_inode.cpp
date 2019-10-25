#include<iostream>
#include <string.h>

using namespace std;


typedef struct seanet_inode{
	char eid[100];
	seanet_inode(){
		strcpy(eid,"");
	}
}seanet_inode;

typedef struct file{
	char name[100];
	char eid[100];
	file(){
		strcpy(name,"");
		strcpy(eid,"");
	}
}file;
char* concat_data(void* d,int length,size_t basic_size){
	char *content = new char[length * basic_size];
	strcat(content,(char*)d);
	return content ;
}

void calculate_eid(char* eid,char* name){
	strcpy(eid,"");
	strcpy(eid,name);
}

int Seanetfs_sendfile(char* filecontent,int flength,int arg, char* rootmfsteid){
	rootmfsteid = "asdfgfasdf";
	return 1;
}
//mode = 0: dir; mode = 1: file
void make_inode(seanet_inode *si,char* d = NULL ,int length = 0, int mode = 0){
	char* content;
	char* rootmfsteid;
	concat_data(d,length,sizeof(file));
	int res = Seanetfs_sendfile(content,length,0,rootmfsteid);
	strcpy(si->eid,rootmfsteid);
	
}
int Seanetfs_sendinode(unsigned char inodeEid[20], char* inodechunk, int ilength){
	return 1;
}
int main (){
	char* file_content = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	
	file *root = new file;
	strcpy(root->name,"/root");
	calculate_eid(root->eid,root->name);//此时root eid 为空。所以root为空文件。
	
	seanet_inode * si = new seanet_inode;
	make_inode(si);
	
	
	file* dirs[3];

	strcpy(dirs[0]->name,"/root/dir1");
	calculate_eid(dirs[0]->eid,dirs[0]->name);
	
	strcpy(dirs[1]->name,"/root/dir2");
	calculate_eid(dirs[1]->eid,dirs[1]->name);

	strcpy(dirs[2]->name,"/root/dir3");
	calculate_eid(dirs[2]->eid,dirs[2]->name);	
	
	
	char* content = concat_data(dirs,3,sizeof(file));
	char* eid = new char[100];
	Seanetfs_sendfile(content,3*sizeof(file),0,eid);
	strcpy(root->eid,eid);
	delete eid;
	
	cout<<root->eid<<endl;
	cout<<dirs[0]->eid<<endl;
}



