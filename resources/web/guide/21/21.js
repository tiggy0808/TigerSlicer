// UNIQUE FUNCTIONS

// Keep in here for future additions
function OnInit()
{
	//let strInput=JSON.stringify(cData);
	//HandleModelList(cData);
	
	TranslatePage();
	RequestProfile();
}
 
function GotoFilamentPage()
{
	let nChoose=OnExitFilter();

	if(nChoose<=0)
		return;

	// Resin printers use resin profiles, not filaments: skip the filament page
	// when only resin printers are selected.
	if(OnlyResinSelected())
		window.open('../5/index.html','_self');
	else
		window.open('../22/index.html','_self');
}

function OnlyResinSelected()
{
	for (let vendor in ModelNozzleSelected)
		for (let model in ModelNozzleSelected[vendor])
			if (ModelNozzleSelected[vendor][model] && vendor != ResinVendor)
				return false;
	return true;
}
