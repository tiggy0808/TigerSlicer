// UNIQUE FUNCTIONS

// Keep in here for future additions
function OnInit()
{
	//let strInput=JSON.stringify(cData);
	//HandleModelList(cData);
	
	TranslatePage();
	RequestProfile();
}
 
function GotoFinishPage()
{
	let nChoose=OnExitFilter();

	if(nChoose<=0)
		return;

	// TigerSlicer: no filament page; every filament in the library is always offered.
	window.open('../5/index.html','_self');
}
