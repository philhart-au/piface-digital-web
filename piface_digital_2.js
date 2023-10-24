function toggle(index) {
  var id = "t" + index;
  var t = document.getElementById(id);
  if ( t.textContent == "0" ) {
    t.textContent = "1";
  } else {
    t.textContent = "0";
  }
const url = "set_bit.qif?"+id+"="+t.textContent;
const options = {
  method: 'PUT',
  headers: {
    'Content-Type': 'application/text'
  },
  body: '0'
};
fetch(url, options);
}
