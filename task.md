check commit c2e91cb86ce013dc0486419f21620975c220c3be and it's related PR.
currently most of the system profiles didn't follow the Bambu's profiles rule of filemant id.
we want to come up a rule for filament id generation for all vendors and a fixing plan for existing system profiles so that there is no ambiguous filament ids for any given printer.

We have two major goals:
### goal 1
figure out a strategy or filament id generation rule. we want to follow bambu's rule but they only care about their own profiles while we need to consider generic. and the filament id generation rule should be easy to understand and maintain for not just us but other profiles creators too. either a determistic rule by using script like setting_id rule. or a documented rule so developers can refer when manually creating profiles. and figure out a way to fix migrate/fix existing filament ids in current profiles. note: bambu profiles shouldn't be touch to maintain interoperbitly with their AMS.

### goal 2
in existing system profiles, the ambiguous can caused by different error patterns:
1.  legit sub type don't have unique filament id due to copy-paste when creating profiles. in this case, we should assign filament unique filament id to it.
2.  generic and specific filament with legit same filament id adds same printer into it's  compatible_printers.  in this case we should remove the printer from the generic filament's compatible_printers.
3. we need to figure out more error patterns and figure out the fix strategy
